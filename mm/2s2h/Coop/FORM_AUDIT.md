# Auditoría de la forma del jugador (`GET_PLAYER_FORM` / `save.playerForm`)

> Documento de análisis para el modo coop (dos Links: el jugador local y un *puppet* remoto, otro actor `Player` con su propio `player->transformation`). **No se ha modificado código.**

## Resumen

### Comandos usados (desde `mm/`)

```sh
grep -rn --include=*.c --include=*.cpp --include=*.h "GET_PLAYER_FORM" src 2s2h include | grep -v "#define GET_PLAYER_FORM"
grep -rn --include=*.c --include=*.cpp --include=*.h "save\.playerForm" src 2s2h include
```

### Recuento exacto

- **`GET_PLAYER_FORM`**: 207 líneas con uso (218 apariciones del token, varias líneas lo usan 2-3 veces) en **72 archivos** (73 si se cuenta `include/z64save.h`, que sólo contiene el `#define` en la línea 570). Incluye 2 líneas que son comentarios (`z_parameter.c:4859`, `GameInteractor_VanillaBehavior.h:1265`) y 5 definiciones de macros derivadas (`Rando/Logic/Logic.h:178-182`).
- **`save.playerForm` (acceso directo)**: el grep devuelve 49 líneas (50 apariciones) en 26 archivos; una de ellas es el propio `#define GET_PLAYER_FORM` (`z64save.h:570`), así que hay **48 líneas de uso** en 26 archivos: **18 lecturas** y **30 escrituras**. `z64save.h:580` (`CUR_FORM`) contiene 2 accesos en la misma línea.
- **Total combinado**: 255 líneas de uso.
- **Nota (hito 2)**: el propio módulo coop agregó después de esta auditoría 2 accesos a `save.playerForm` que no están en la tabla: `Coop/CoopPuppet.cpp` (lectura para el log de pruebas `Self sample`, categoría A) y `Coop/CoopTest.cpp` (escritura de `--coop-test-form`, la misma transformación de `FastTransformation.cpp`, categoría B). Con ellos el grep da 51 líneas.

### Por categoría

Categorías: **A** = HUD/menú/UI/presentación local o contabilidad del save del jugador local · **B** = lógica del propio Link (código de `Player`, donde `this->transformation` es equivalente) · **C** = lógica del mundo que reacciona a la forma "del jugador" (NPCs, enemigos, puertas, cofres, cutscenes, diálogos, checks) y que debería leer la forma del jugador **relevante**. "(?)" = clasificación dudosa (ver nota de la fila).

| Conjunto | A | B | C | Total |
|---|---|---|---|---|
| `GET_PLAYER_FORM` | 55 (6 ?) | 16 (13 ?) | 136 (6 ?) | 207 |
| `save.playerForm` (lecturas + escrituras) | 18 | 7 (1 ?) | 23 (9 ?) | 48 |
| &nbsp;&nbsp;de ellas escrituras | 14 | 7 | 9 | 30 |
| &nbsp;&nbsp;de ellas lecturas | 4 | 0 | 14 | 18 |
| **Combinado** | **73 (6 ?)** | **23 (14 ?)** | **159 (15 ?)** | **255** |

Los números entre paréntesis indican cuántos de esa categoría están marcados como dudosos "(?)". La línea `z64save.h:570` (definición) no se cuenta.

Observaciones rápidas:

- La gran mayoría de los usos en actores (`src/overlays/actors/ovl_En_*`, `ovl_Bg_*`, `ovl_Obj_*`) son **C**: casi todos son diálogos (elección de `textId`), condiciones de minijuego o de enemigos que agarran/esquivan al jugador. Muchos ya tienen un `Player* player = GET_PLAYER(play)` a mano, por lo que el cambio sería mecánico una vez que se decida *qué* jugador es el relevante.
- Los usos **B** dentro de `ovl_player_actor`/`z_player_lib` se concentran en el ciclo de **transformación con máscara**: el save guarda la forma *destino* mientras `this->transformation` sigue siendo la vieja (ver caveats).
- Los usos **A** (kaleido, interfaz, save editor, map select, carga/guardado) deben quedarse en la forma global: describen al jugador local.

## Escrituras a `save.playerForm`

| Archivo | Línea | Cat. | Propósito |
|---|---|---|---|
| `2s2h/Coop/CoopSnapshot.cpp` | 40 | A | Coop: RestorePerPlayer restaura la forma del jugador local. |
| `2s2h/Coop/CoopTest.cpp` | 80 | A | Coop test: BootToFileSelect, Humano. |
| `2s2h/DeveloperTools/SaveEditor.cpp` | 1604 | A | Save editor: cambiar forma del jugador local. |
| `2s2h/DeveloperTools/WarpPoint.cpp` | 57 | A | Warp de debug: Humano. |
| `2s2h/Enhancements/Cutscenes/SkipIntroSequence.cpp` | 28 | C (?) | Skip intro: Deku (equivale a comando de cutscene). |
| `2s2h/Enhancements/Cutscenes/SkipIntroSequence.cpp` | 39 | C (?) | Skip intro (primer ciclo): Humano. |
| `2s2h/Enhancements/Cutscenes/SkipToFileSelect.cpp` | 21 | A | Arranque directo a file select: Humano. |
| `2s2h/Enhancements/Cutscenes/StoryCutscenes/SkipFirstSongOfTimeFlashbacks.cpp` | 21 | C (?) | Skip flashbacks: Deku (como haría el cutscene). |
| `2s2h/Enhancements/Cutscenes/StoryCutscenes/SkipLearningSongOfHealing.cpp` | 43 | B (?) | Skip Canción de Curación: Humano + recarga del Player local. |
| `2s2h/Enhancements/Cutscenes/StoryCutscenes/SkipLearningSongOfTime.cpp` | 24 | C (?) | Skip Canción del Tiempo: Deku (como haría el cutscene). |
| `2s2h/Rando/MiscBehavior/OnFileCreate.cpp` | 34 | A | Rando: archivo nuevo empieza Humano. |
| `2s2h/SaveManager/BinarySaveConverter.cpp` | 503 | A | Conversión de save binario legacy (carga). |
| `src/code/z_common_data.c` | 15 | A | SaveContext_Init: reset a 0. |
| `src/code/z_demo.c` | 317 | C (?) | Cutscene fuerza Deku (CS_MISC_PLAYER_FORM_DEKU). |
| `src/code/z_demo.c` | 330 | C (?) | Cutscene fuerza Humano (guarda la anterior en :329). |
| `src/code/z_demo.c` | 360 | C (?) | Cutscene restaura la forma guardada. |
| `src/code/z_demo.c` | 414 | C (?) | Cutscene fuerza Zora. |
| `src/code/z_game_over.c` | 103 | A | Game over: vuelve a Humano si tiene la máscara Deku (save del jugador local). |
| `src/code/z_sram_NES.c` | 994 | A | Sram_InitNewSave: Humano. |
| `src/code/z_sram_NES.c` | 1403 | A | Sram_OpenSave: Humano al reiniciar al cutscene inicial. |
| `src/overlays/actors/ovl_Dm_Char02/z_dm_char02.c` | 157 | C (?) | Recoger la Ocarina del Tiempo (fin del primer ciclo): fuerza Humano. Evento de historia; aplicar al jugador que la recoge. |
| `src/overlays/actors/ovl_player_actor/z_player.c` | 6157 | B | func_808345C8: al reaparecer/morir vuelve a Humano si tiene máscara Deku. |
| `src/overlays/actors/ovl_player_actor/z_player.c` | 7784 | B | func_808388B8: fija la forma DESTINO al empezar la transformación con máscara (this->transformation sigue siendo la vieja hasta la recarga en func_8012301C). |
| `src/overlays/actors/ovl_player_actor/z_player.c` | 20969 | B | Player_CsAction_6: cutscene fuerza Humano y recarga vía func_8012301C. |
| `src/overlays/actors/ovl_player_actor/z_player.c` | 21137 | B | Player_CsAction_22: cutscene fuerza Deku. |
| `src/overlays/actors/ovl_player_actor/z_player.c` | 21167 | B | Player_CsAction_25: cutscene fuerza Fierce Deity. |
| `src/overlays/actors/ovl_player_actor/z_player.c` | 21333 | B | Player_CsAction_43: cutscene fuerza Humano y recarga. |
| `src/overlays/gamestates/ovl_opening/z_opening.c` | 37 | A | Pantalla de título: Humano. |
| `src/overlays/gamestates/ovl_select/z_select.c` | 574 | A | Map Select: aplicar forma elegida (debug). |
| `src/overlays/gamestates/ovl_select/z_select.c` | 1142 | A | Map Select init: Humano. |

Notas sobre escrituras:

- En coop, cualquier escritura cambia la forma del **jugador local** (el save es local). Las del grupo **B** ocurren dentro de código de `Player`: si ese código se ejecuta para el puppet (p. ej. si el puppet corre `Player_Action_86` o un `Player_CsAction_*`), **escribiría la forma del jugador local**. El puppet no debe ejecutar esas rutas o deben redirigirse a un campo por-Player.
- `z_demo.c` (comandos de cutscene `CS_MISC_PLAYER_FORM_*`) y los *skips* de cutscene equivalentes fuerzan la forma del protagonista del cutscene; en coop hay que decidir si aplican sólo al jugador que lo dispara.
- `2s2h/Coop/CoopSnapshot.cpp` ya captura/restaura `playerForm` como dato por-jugador: es el sitio natural para sincronizar la forma del jugador local hacia el remoto.

## Tabla completa

Ordenada por archivo y línea. `· W` = escritura. Las filas con `GET_PLAYER_FORM` en el snippet son usos de la macro; las demás son accesos directos a `save.playerForm`.

| # | Archivo | Línea | Uso | Categoría | Nota |
|---|---|---|---|---|---|
| 1 | `2s2h/Coop/CoopSnapshot.cpp` | 30 | `p.playerForm = gSaveContext.save.playerForm;` | A | Coop: CapturePerPlayer guarda la forma del jugador local (lectura). |
| 2 | `2s2h/Coop/CoopSnapshot.cpp` | 40 | `gSaveContext.save.playerForm = p.playerForm;` | A · W | Coop: RestorePerPlayer restaura la forma del jugador local. |
| 3 | `2s2h/Coop/CoopTest.cpp` | 80 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | A · W | Coop test: BootToFileSelect, Humano. |
| 4 | `2s2h/CustomItem/CustomItem.cpp` | 169 | `switch (GET_PLAYER_FORM) {` | C (?) | CustomItem00_Update: altura sobre la cabeza del jugador que recoge el ítem; posición tomada de GET_PLAYER(play). Debería ser el jugador que lo recogió. |
| 5 | `2s2h/CustomItem/CustomItem.cpp` | 222 | `switch (GET_PLAYER_FORM) {` | C (?) | Idem (rama HIDE_TILL_OVERHEAD). |
| 6 | `2s2h/DeveloperTools/BetterMapSelect.c` | 359 | `GfxPrint_Printf(printer, "%s", betterFormLabels[GET_PLAYER_FORM]);` | A | Better Map Select: imprime la forma (debug). |
| 7 | `2s2h/DeveloperTools/SaveEditor.cpp` | 1229 | `if (GET_PLAYER_FORM != PLAYER_FORM_FIERCE_DEITY) {` | A | Save editor: no cambiar espada si FD. |
| 8 | `2s2h/DeveloperTools/SaveEditor.cpp` | 1248 | `if (GET_PLAYER_FORM != PLAYER_FORM_FIERCE_DEITY) {` | A | Save editor: no cambiar espada si FD. |
| 9 | `2s2h/DeveloperTools/SaveEditor.cpp` | 1282 | `if (GET_PLAYER_FORM == PLAYER_FORM_FIERCE_DEITY) {` | A | Save editor: icono de espada FD. |
| 10 | `2s2h/DeveloperTools/SaveEditor.cpp` | 1516 | `if (form != 0 \|\| GET_PLAYER_FORM == PLAYER_FORM_FIERCE_DEITY) {` | A | Save editor: ClearAllEquippedItems respeta B de FD. |
| 11 | `2s2h/DeveloperTools/SaveEditor.cpp` | 1571 | `GetPlayerForm(GET_PLAYER_FORM);` | A | Save editor: muestra la forma. |
| 12 | `2s2h/DeveloperTools/SaveEditor.cpp` | 1604 | `gSaveContext.save.playerForm = i;` | A · W | Save editor: cambiar forma del jugador local. |
| 13 | `2s2h/DeveloperTools/SaveEditor.cpp` | 1608 | `player->actor.shape.rot.z = GET_PLAYER_FORM + 1;` | A | Save editor: cambio de forma del jugador local (rot.z = forma+1 antes de re-Init). |
| 14 | `2s2h/DeveloperTools/SaveEditor.cpp` | 1626 | `if (GET_PLAYER_FORM == PLAYER_FORM_HUMAN) {` | A | Save editor: combos de equipo sólo Humano. |
| 15 | `2s2h/DeveloperTools/SaveEditor.cpp` | 1723 | `GetPlayerForm(GET_PLAYER_FORM);` | A | Save editor: muestra la forma en tabla de estado. |
| 16 | `2s2h/DeveloperTools/WarpPoint.cpp` | 57 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | A · W | Warp de debug: Humano. |
| 17 | `2s2h/Enhancements/Cutscenes/SkipIntroSequence.cpp` | 28 | `gSaveContext.save.playerForm = PLAYER_FORM_DEKU;` | C (?) · W | Skip intro: Deku (equivale a comando de cutscene). |
| 18 | `2s2h/Enhancements/Cutscenes/SkipIntroSequence.cpp` | 39 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | C (?) · W | Skip intro (primer ciclo): Humano. |
| 19 | `2s2h/Enhancements/Cutscenes/SkipToFileSelect.cpp` | 21 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | A · W | Arranque directo a file select: Humano. |
| 20 | `2s2h/Enhancements/Cutscenes/StoryCutscenes/SkipFirstSongOfTimeFlashbacks.cpp` | 21 | `gSaveContext.save.playerForm = PLAYER_FORM_DEKU;` | C (?) · W | Skip flashbacks: Deku (como haría el cutscene). |
| 21 | `2s2h/Enhancements/Cutscenes/StoryCutscenes/SkipLearningSongOfHealing.cpp` | 43 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | B (?) · W | Skip Canción de Curación: Humano + recarga del Player local. |
| 22 | `2s2h/Enhancements/Cutscenes/StoryCutscenes/SkipLearningSongOfHealing.cpp` | 46 | `player->actor.shape.rot.z = GET_PLAYER_FORM + 1;` | B (?) | Skip de cutscene: re-inicializa el Player local tras forzar Humano (copia de func_8012301C). |
| 23 | `2s2h/Enhancements/Cutscenes/StoryCutscenes/SkipLearningSongOfTime.cpp` | 24 | `gSaveContext.save.playerForm = PLAYER_FORM_DEKU;` | C (?) · W | Skip Canción del Tiempo: Deku (como haría el cutscene). |
| 24 | `2s2h/Enhancements/Masks/EquipWhileSwimming.cpp` | 18 | `if (GET_PLAYER_FORM == PLAYER_FORM_HUMAN && item > ITEM_MASK_FIERCE_DEITY && item < ITEM_MAS...` | A | Hook VB_DISABLE_ITEM_UNDERWATER (z_parameter.c, botones del HUD local). |
| 25 | `2s2h/Enhancements/Masks/FastTransformation.cpp` | 38 | `return (player->transformation < PLAYER_FORM_HUMAN) \|\| CHECK_WEEKEVENTREG(sFormCheckFlags[GE...` | B | ShouldFastTransform: ya usa player->transformation; la forma destino viene del save. Requiere forma destino por-Player. |
| 26 | `2s2h/Enhancements/Masks/FastTransformation.cpp` | 51 | `s16 objectId = gPlayerFormObjectIds[GET_PLAYER_FORM];` | B (?) | Copia de func_8012301C para transformación rápida (objeto de la forma destino global). |
| 27 | `2s2h/Enhancements/Masks/FastTransformation.cpp` | 59 | `player->actor.shape.rot.z = GET_PLAYER_FORM + 1;` | B (?) | Copia de func_8012301C: rot.z = forma+1. |
| 28 | `2s2h/Enhancements/Masks/FierceDeityAnywhere.cpp` | 208 | `if (GET_PLAYER_FORM == PLAYER_FORM_FIERCE_DEITY && fabsf(playerZPosRelToDoor) < 60.0f) {` | C | Hook VB_BE_NEAR_DOOR (EnDoor/DoorShutter): proximidad ampliada si FD; usar la forma del jugador que abre la puerta. |
| 29 | `2s2h/Enhancements/Masks/PersistentMasks.cpp` | 158 | `(GET_PLAYER_FORM == PLAYER_FORM_HUMAN \|\| gPlayState->actorCtx.isOverrideInputOn == 0)) {` | B | Hook VB_CONSIDER_BUNNY_HOOD_EQUIPPED recibe `Player* player`: usar player->transformation. |
| 30 | `2s2h/GameInteractor/GameInteractor_VanillaBehavior.h` | 1265 | `// !gPlayerFormItemRestrictions[GET_PLAYER_FORM][itemId]` | A | Comentario de documentación de VB_ITEM_BE_RESTRICTED (no ejecutable). |
| 31 | `2s2h/Rando/Logic/Logic.h` | 178 | `#define IS_DEKU (GET_PLAYER_FORM == PLAYER_FORM_DEKU)` | A (?) | Macro IS_DEKU de la lógica de alcanzabilidad del rando (vía CAN_BE_*, ~513 usos en Regions/*.cpp). Evalúa el save local (lógica/tracker), no una interacción del mundo. |
| 32 | `2s2h/Rando/Logic/Logic.h` | 179 | `#define IS_ZORA (GET_PLAYER_FORM == PLAYER_FORM_ZORA)` | A (?) | Macro IS_ZORA de la lógica de alcanzabilidad del rando (vía CAN_BE_*, ~513 usos en Regions/*.cpp). Evalúa el save local (lógica/tracker), no una interacción del mundo. |
| 33 | `2s2h/Rando/Logic/Logic.h` | 180 | `#define IS_DEITY (GET_PLAYER_FORM == PLAYER_FORM_FIERCE_DEITY)` | A (?) | Macro IS_DEITY de la lógica de alcanzabilidad del rando (vía CAN_BE_*, ~513 usos en Regions/*.cpp). Evalúa el save local (lógica/tracker), no una interacción del mundo. |
| 34 | `2s2h/Rando/Logic/Logic.h` | 181 | `#define IS_GORON (GET_PLAYER_FORM == PLAYER_FORM_GORON)` | A (?) | Macro IS_GORON de la lógica de alcanzabilidad del rando (vía CAN_BE_*, ~513 usos en Regions/*.cpp). Evalúa el save local (lógica/tracker), no una interacción del mundo. |
| 35 | `2s2h/Rando/Logic/Logic.h` | 182 | `#define IS_HUMAN (GET_PLAYER_FORM == PLAYER_FORM_HUMAN)` | A (?) | Macro IS_HUMAN de la lógica de alcanzabilidad del rando (vía CAN_BE_*, ~513 usos en Regions/*.cpp). Evalúa el save local (lógica/tracker), no una interacción del mundo. |
| 36 | `2s2h/Rando/MiscBehavior/MiscBehavior.cpp` | 46 | `if (GET_PLAYER_FORM != PLAYER_FORM_FIERCE_DEITY) {` | A | Rando: al dar espada no tocar el botón B si FD (equipos del save). |
| 37 | `2s2h/Rando/MiscBehavior/OnFileCreate.cpp` | 34 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | A · W | Rando: archivo nuevo empieza Humano. |
| 38 | `2s2h/SaveManager/BinarySaveConverter.cpp` | 464 | `{ "playerForm", save.playerForm },` | A | Serialización JSON del save legacy (lectura). |
| 39 | `2s2h/SaveManager/BinarySaveConverter.cpp` | 503 | `saveContext->save.playerForm = reader->ReadUByte();` | A · W | Conversión de save binario legacy (carga). |
| 40 | `2s2h/z_scene_2SH.cpp` | 64 | `playerObjectId = gPlayerFormObjectIds[GET_PLAYER_FORM];` | B (?) | Versión 2S2H de Scene_CommandSpawnList: carga el objeto de la forma del jugador local (igual que z_scene.c:182). |
| 41 | `include/z64save.h` | 570 | `#define GET_PLAYER_FORM ((void)0, gSaveContext.save.playerForm)` | — (definición) | Definición de GET_PLAYER_FORM (no es un uso). |
| 42 | `include/z64save.h` | 580 | `#define CUR_FORM ((gSaveContext.save.playerForm == PLAYER_FORM_HUMAN) ? 0 : gSaveContext.sav...` | A | Macro CUR_FORM (2 accesos en la línea). Sus ~80 usos (CUR_FORM, CUR_FORM_EQUIP, GET_CUR_FORM_BTN_*) heredan esta semántica: botones/equipos del jugador local. |
| 43 | `src/code/z_common_data.c` | 15 | `gSaveContext.save.playerForm = 0;` | A · W | SaveContext_Init: reset a 0. |
| 44 | `src/code/z_demo.c` | 317 | `gSaveContext.save.playerForm = PLAYER_FORM_DEKU;` | C (?) · W | Cutscene fuerza Deku (CS_MISC_PLAYER_FORM_DEKU). |
| 45 | `src/code/z_demo.c` | 329 | `sCutsceneStoredPlayerForm = GET_PLAYER_FORM;` | C (?) | CutsceneCmd_Misc CS_MISC_PLAYER_FORM_HUMAN: guarda la forma antes de forzar Humano (ver escritura en :330). Afecta al protagonista del cutscene; en coop decidir a qué jugador aplica. |
| 46 | `src/code/z_demo.c` | 330 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | C (?) · W | Cutscene fuerza Humano (guarda la anterior en :329). |
| 47 | `src/code/z_demo.c` | 360 | `gSaveContext.save.playerForm = sCutsceneStoredPlayerForm;` | C (?) · W | Cutscene restaura la forma guardada. |
| 48 | `src/code/z_demo.c` | 414 | `gSaveContext.save.playerForm = PLAYER_FORM_ZORA;` | C (?) · W | Cutscene fuerza Zora. |
| 49 | `src/code/z_en_item00.c` | 1358 | `switch (GET_PLAYER_FORM) {` | C (?) | Item_DropCollectibleRandom: el drop 'máscara' se convierte según la forma. Debería usar la forma del jugador que provocó el drop (o el más cercano). |
| 50 | `src/code/z_game_over.c` | 103 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | A · W | Game over: vuelve a Humano si tiene la máscara Deku (save del jugador local). |
| 51 | `src/code/z_message.c` | 4590 | `if (gSaveContext.save.playerForm == 4) {}` | A | FAKE match (`if (...) {}` vacío) junto a la selección de instrumento de ocarina por CUR_FORM; sin efecto. |
| 52 | `src/code/z_parameter.c` | 3357 | `if (GET_PLAYER_FORM == player->transformation) {` | A | Interface_UpdateButtonsPart2: compara con player->transformation del jugador local (GET_PLAYER) para habilitar botones C. |
| 53 | `src/code/z_parameter.c` | 3365 | `if (GameInteractor_Should(VB_ITEM_BE_RESTRICTED, !gPlayerFormItemRestrictions[GET_PLAYER_FOR...` | A | Restricción de ítems en botones C según forma (HUD local). |
| 54 | `src/code/z_parameter.c` | 3512 | `if (GameInteractor_Should(VB_ITEM_BE_RESTRICTED, !gPlayerFormItemRestrictions[GET_PLAYER_FOR...` | A | Restricción de ítems en D-pad según forma (HUD local, 2S2H). |
| 55 | `src/code/z_parameter.c` | 4859 | `// Is simply checking if (GET_PLAYER_FORM == PLAYER_FORM_FIERCE_DEITY)` | A | Comentario (no ejecutable) en Inventory_UpdateDeitySwordEquip. |
| 56 | `src/code/z_parameter.c` | 4860 | `if ((((GET_PLAYER_FORM > 0) && (GET_PLAYER_FORM < 4)) ? 1 : GET_PLAYER_FORM >> 1) == 0) {` | A (?) | Inventory_UpdateDeitySwordEquip: pone/quita la espada FD en el botón B del save. Se llama desde Player_Init: si el puppet ejecuta Player_Init tocaría los equipos del save local. |
| 57 | `src/code/z_parameter.c` | 9336 | `if (player->transformation == GET_PLAYER_FORM) {` | A | Interface_Draw: dibuja iconos del botón B sólo si la forma del save coincide con la del Player local. |
| 58 | `src/code/z_player_lib.c` | 407 | `s16 objectId = gPlayerFormObjectIds[GET_PLAYER_FORM];` | B (?) | func_8012301C (update temporal tras transformación): carga el objeto de la forma DESTINO leída del save. Para el puppet hace falta una forma destino propia (no la global). |
| 59 | `src/code/z_player_lib.c` | 417 | `this->actor.shape.rot.z = GET_PLAYER_FORM + 1;` | B (?) | func_8012301C: rot.z = forma+1 antes de re-Init (Player_Init sólo lo usa como flag !=0 para colores de estela). Mismo problema que :407. |
| 60 | `src/code/z_player_lib.c` | 1678 | `return sPlayerStrengths[GET_PLAYER_FORM];` | B (?) | Player_GetStrength(void) no recibe Player*. Llamado desde z_player.c:7414 (B) y ovl_Obj_Oshihiki:86 (C, bloque empujable). Conviene una variante Player_GetStrengthOf(player). |
| 61 | `src/code/z_scene.c` | 182 | `playerObjectId = gPlayerFormObjectIds[GET_PLAYER_FORM];` | B (?) | Scene_CommandSpawnList: carga como objeto persistente el objeto de la forma del jugador LOCAL. Un puppet con otra forma necesita también su objeto (OBJECT_LINK_*) cargado. |
| 62 | `src/code/z_sram_NES.c` | 994 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | A · W | Sram_InitNewSave: Humano. |
| 63 | `src/code/z_sram_NES.c` | 1223 | `if (GET_PLAYER_FORM != PLAYER_FORM_HUMAN) {` | A | Sram_InitDebugSave: equipa el ítem de forma en C-abajo del save de debug. |
| 64 | `src/code/z_sram_NES.c` | 1224 | `BUTTON_ITEM_EQUIP(0, EQUIP_SLOT_C_DOWN) = D_801C6A48[GET_PLAYER_FORM];` | A | Sram_InitDebugSave: equipa el ítem de forma en C-abajo del save de debug. |
| 65 | `src/code/z_sram_NES.c` | 1225 | `C_SLOT_EQUIP(0, EQUIP_SLOT_C_DOWN) = D_801C6A50[GET_PLAYER_FORM];` | A | Sram_InitDebugSave: equipa el ítem de forma en C-abajo del save de debug. |
| 66 | `src/code/z_sram_NES.c` | 1403 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | A · W | Sram_OpenSave: Humano al reiniciar al cutscene inicial. |
| 67 | `src/overlays/actors/ovl_Bg_Danpei_Movebg/z_bg_danpei_movebg.c` | 125 | `if (gSaveContext.save.playerForm == PLAYER_FORM_DEKU) {` | C | Plataforma móvil (Dampé): reacción si el jugador encima es Deku. Usar la forma del jugador que está encima. |
| 68 | `src/overlays/actors/ovl_Bg_Lotus/z_bg_lotus.c` | 86 | `if (gSaveContext.save.playerForm != PLAYER_FORM_DEKU) {` | C | Nenúfar: se hunde si quien está encima no es Deku. |
| 69 | `src/overlays/actors/ovl_Demo_Tre_Lgt/z_demo_tre_lgt.c` | 71 | `if (GET_PLAYER_FORM == PLAYER_FORM_FIERCE_DEITY) {` | C | Luz del cofre grande: tipo de animación según la forma de quien abre el cofre. |
| 70 | `src/overlays/actors/ovl_Dm_Char02/z_dm_char02.c` | 157 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | C (?) · W | Recoger la Ocarina del Tiempo (fin del primer ciclo): fuerza Humano. Evento de historia; aplicar al jugador que la recoge. |
| 71 | `src/overlays/actors/ovl_En_Bal/z_en_bal.c` | 513 | `if (GET_PLAYER_FORM == EnBal_GetRecognizedPlayerForm()) {` | C | Tingle reconoce la forma con la que se le habló (diálogo). |
| 72 | `src/overlays/actors/ovl_En_Bal/z_en_bal.c` | 662 | `switch (gSaveContext.save.playerForm) {` | C | EnBal_SetRecognizedPlayerForm: Tingle guarda en weekEventReg la forma con la que habló. |
| 73 | `src/overlays/actors/ovl_En_Bji_01/z_en_bji_01.c` | 125 | `switch (GET_PLAYER_FORM) {` | C | Shikashi (astrónomo): texto según forma. |
| 74 | `src/overlays/actors/ovl_En_Bji_01/z_en_bji_01.c` | 166 | `switch (GET_PLAYER_FORM) {` | C | Shikashi (astrónomo): texto según forma. |
| 75 | `src/overlays/actors/ovl_En_Bji_01/z_en_bji_01.c` | 244 | `switch (GET_PLAYER_FORM) {` | C | Shikashi (astrónomo): texto según forma. |
| 76 | `src/overlays/actors/ovl_En_Box/z_en_box.c` | 471 | `anim = sBigChestAnimations[GET_PLAYER_FORM];` | C | Cofre grande: animación de apertura según forma del que abre. |
| 77 | `src/overlays/actors/ovl_En_Box/z_en_box.c` | 472 | `playSpeed = sPlaybackSpeeds[GET_PLAYER_FORM];` | C | Cofre grande: velocidad de animación según forma del que abre. |
| 78 | `src/overlays/actors/ovl_En_Box/z_en_box.c` | 554 | `if (Animation_OnFrame(&this->skelAnime, GET_PLAYER_FORM == PLAYER_FORM_DEKU ? 14.0f : 30.0f)) {` | C | Cofre: frame del SFX de desbloqueo según forma (Deku). |
| 79 | `src/overlays/actors/ovl_En_Box/z_en_box.c` | 556 | `} else if (Animation_OnFrame(&this->skelAnime, GET_PLAYER_FORM == PLAYER_FORM_DEKU ? 15.0f :...` | C | Cofre: frame del SFX de apertura según forma (Deku). |
| 80 | `src/overlays/actors/ovl_En_Dinofos/z_en_dinofos.c` | 483 | `if ((GET_PLAYER_FORM == PLAYER_FORM_GORON) && (player->actor.velocity.y < -5.0f) && (player-...` | C | Esquiva el golpe de suelo Goron; ya usa `player` (target) para el resto de la condición. |
| 81 | `src/overlays/actors/ovl_En_Elfgrp/z_en_elfgrp.c` | 634 | `if (GET_PLAYER_FORM == PLAYER_FORM_DEKU) {` | C | Fuente de las Grandes Hadas: comportamiento si el jugador cercano es Deku. |
| 82 | `src/overlays/actors/ovl_En_Elforg/z_en_elforg.c` | 389 | `if (GET_PLAYER_FORM == PLAYER_FORM_GORON) {` | C | Hada perdida orbita al jugador: radio según forma (Goron). |
| 83 | `src/overlays/actors/ovl_En_Floormas/z_en_floormas.c` | 718 | `ptr = &D_808D3900[GET_PLAYER_FORM];` | C | func_808D217C ya recibe `Player* player`: usar PLAYER_FORM_OF(player) directamente. |
| 84 | `src/overlays/actors/ovl_En_Fu/z_en_fu.c` | 133 | `if ((GET_PLAYER_FORM == PLAYER_FORM_DEKU) && (CURRENT_DAY == 3)) {` | C | Honey & Darling: Deku en día 3. |
| 85 | `src/overlays/actors/ovl_En_Fu/z_en_fu.c` | 408 | `if (GET_PLAYER_FORM == PLAYER_FORM_DEKU) {` | C | Honey & Darling: Diálogo según forma. |
| 86 | `src/overlays/actors/ovl_En_Fu/z_en_fu.c` | 679 | `if ((GET_PLAYER_FORM == PLAYER_FORM_DEKU) && gSaveContext.save.saveInfo.playerData.isMagicAc...` | C | Honey & Darling: Rellena magia si Deku. |
| 87 | `src/overlays/actors/ovl_En_Fu/z_en_fu.c` | 774 | `if (GET_PLAYER_FORM == PLAYER_FORM_HUMAN) {` | C | Honey & Darling: Activa flag de estado del jugador si Humano. |
| 88 | `src/overlays/actors/ovl_En_Fu/z_en_fu.c` | 855 | `if (GET_PLAYER_FORM == PLAYER_FORM_DEKU) {` | C | Honey & Darling: func_809632D0: visibilidad del HUD si Deku (efecto UI, pero decidido por el minijuego). |
| 89 | `src/overlays/actors/ovl_En_Fu/z_en_fu.c` | 931 | `(GET_PLAYER_FORM == PLAYER_FORM_HUMAN)) {` | C | Honey & Darling: Diálogo/premio día 3 Humano. |
| 90 | `src/overlays/actors/ovl_En_Fu/z_en_fu.c` | 952 | `if (GET_PLAYER_FORM == PLAYER_FORM_HUMAN) {` | C | Honey & Darling: Marca weekEventReg de victoria según forma Humano. |
| 91 | `src/overlays/actors/ovl_En_Fu/z_en_fu.c` | 1053 | `if (GET_PLAYER_FORM == PLAYER_FORM_HUMAN) {` | C | Honey & Darling: Diálogo día 1. |
| 92 | `src/overlays/actors/ovl_En_Fu/z_en_fu.c` | 1075 | `if (GET_PLAYER_FORM != PLAYER_FORM_HUMAN) {` | C | Honey & Darling: Diálogo día 2. |
| 93 | `src/overlays/actors/ovl_En_Fu/z_en_fu.c` | 1104 | `if (GET_PLAYER_FORM != PLAYER_FORM_HUMAN) {` | C | Honey & Darling: Diálogo día 3. |
| 94 | `src/overlays/actors/ovl_En_Fu/z_en_fu.c` | 1105 | `if (GET_PLAYER_FORM == PLAYER_FORM_DEKU) {` | C | Honey & Darling: Día 3: rama Deku. |
| 95 | `src/overlays/actors/ovl_En_Fu/z_en_fu.c` | 1158 | `if (GET_PLAYER_FORM != PLAYER_FORM_HUMAN) {` | C | Honey & Darling: Diálogo según forma. |
| 96 | `src/overlays/actors/ovl_En_Fu_Mato/z_en_fu_mato.c` | 370 | `if ((fu->unk_542 == 2) \|\| (GET_PLAYER_FORM == PLAYER_FORM_DEKU)) {` | C | Diana del minijuego Honey & Darling: regla distinta para Deku. |
| 97 | `src/overlays/actors/ovl_En_Gs/z_en_gs.c` | 315 | `if ((this->actor.params == ENGS_1) && (GET_PLAYER_FORM == PLAYER_FORM_DEKU)) {` | C | Piedra chismosa: canción + forma (Sonata/Deku); usar la forma de quien toca la ocarina. |
| 98 | `src/overlays/actors/ovl_En_Gs/z_en_gs.c` | 325 | `if ((this->actor.params == ENGS_1) && (GET_PLAYER_FORM == PLAYER_FORM_ZORA)) {` | C | Piedra chismosa: canción + forma (Nueva Onda/Zora); usar la forma de quien toca la ocarina. |
| 99 | `src/overlays/actors/ovl_En_Gs/z_en_gs.c` | 335 | `if ((this->actor.params == ENGS_1) && (GET_PLAYER_FORM == PLAYER_FORM_GORON)) {` | C | Piedra chismosa: canción + forma (Nana Goron/Goron); usar la forma de quien toca la ocarina. |
| 100 | `src/overlays/actors/ovl_En_Hg/z_en_hg.c` | 401 | `if ((play->msgCtx.lastPlayedSong == OCARINA_SONG_HEALING) && (GET_PLAYER_FORM == PLAYER_FORM...` | C | Padre de Pamela: Canción de Curación tocada por Humano (check rando vía VB). |
| 101 | `src/overlays/actors/ovl_En_Hgo/z_en_hgo.c` | 161 | `} else if (GET_PLAYER_FORM == PLAYER_FORM_HUMAN) {` | C | Padre de Pamela (Hgo): diálogo según forma. |
| 102 | `src/overlays/actors/ovl_En_In/z_en_in.c` | 489 | `if ((GET_PLAYER_FORM == PLAYER_FORM_ZORA) \|\| (GET_PLAYER_FORM == PLAYER_FORM_GORON)) {` | C | Gorman (rancho): textId según forma. |
| 103 | `src/overlays/actors/ovl_En_In/z_en_in.c` | 491 | `} else if (GET_PLAYER_FORM == PLAYER_FORM_DEKU) {` | C | Gorman (rancho): textId según forma. |
| 104 | `src/overlays/actors/ovl_En_In/z_en_in.c` | 509 | `if (GET_PLAYER_FORM == PLAYER_FORM_DEKU) {` | C | Gorman (rancho): textId según forma. |
| 105 | `src/overlays/actors/ovl_En_In/z_en_in.c` | 511 | `} else if ((GET_PLAYER_FORM == PLAYER_FORM_ZORA) \|\| (GET_PLAYER_FORM == PLAYER_FORM_GORON)) {` | C | Gorman (rancho): textId según forma. |
| 106 | `src/overlays/actors/ovl_En_In/z_en_in.c` | 521 | `if ((GET_PLAYER_FORM == PLAYER_FORM_ZORA) \|\| (GET_PLAYER_FORM == PLAYER_FORM_GORON)) {` | C | Gorman (rancho): textId según forma. |
| 107 | `src/overlays/actors/ovl_En_In/z_en_in.c` | 523 | `} else if (GET_PLAYER_FORM == PLAYER_FORM_DEKU) {` | C | Gorman (rancho): textId según forma. |
| 108 | `src/overlays/actors/ovl_En_In/z_en_in.c` | 543 | `if (GET_PLAYER_FORM == PLAYER_FORM_DEKU) {` | C | Gorman (rancho): textId según forma. |
| 109 | `src/overlays/actors/ovl_En_In/z_en_in.c` | 545 | `} else if ((GET_PLAYER_FORM == PLAYER_FORM_ZORA) \|\| (GET_PLAYER_FORM == PLAYER_FORM_GORON)) {` | C | Gorman (rancho): textId según forma. |
| 110 | `src/overlays/actors/ovl_En_Jgame_Tsn/z_en_jgame_tsn.c` | 202 | `(this->unk_2FE == 0) && (GET_PLAYER_FORM == PLAYER_FORM_HUMAN) && func_80C149B0(play, &this-...` | C | Minijuego de pesca (Great Bay): sólo Humano. |
| 111 | `src/overlays/actors/ovl_En_Js/z_en_js.c` | 662 | `if (GET_PLAYER_FORM != PLAYER_FORM_HUMAN) {` | C | Niño de la Luna (Majora child): texto según forma. |
| 112 | `src/overlays/actors/ovl_En_Js/z_en_js.c` | 856 | `if (GET_PLAYER_FORM != PLAYER_FORM_HUMAN) {` | C | Niño de la Luna (Majora child): texto según forma. |
| 113 | `src/overlays/actors/ovl_En_Js/z_en_js.c` | 886 | `if (GET_PLAYER_FORM != PLAYER_FORM_HUMAN) {` | C | Niño de la Luna (Majora child): texto según forma. |
| 114 | `src/overlays/actors/ovl_En_Kbt/z_en_kbt.c` | 332 | `if ((GET_PLAYER_FORM != PLAYER_FORM_HUMAN) \|\|` | C | Herrero (Zubora): diálogo/servicio sólo para Humano. |
| 115 | `src/overlays/actors/ovl_En_Kbt/z_en_kbt.c` | 351 | `if (GET_PLAYER_FORM != PLAYER_FORM_HUMAN) {` | C | Herrero (Zubora): diálogo/servicio sólo para Humano. |
| 116 | `src/overlays/actors/ovl_En_Kbt/z_en_kbt.c` | 357 | `if (GET_PLAYER_FORM == PLAYER_FORM_HUMAN) {` | C | Herrero (Zubora): diálogo/servicio sólo para Humano. |
| 117 | `src/overlays/actors/ovl_En_Kendo_Js/z_en_kendo_js.c` | 205 | `} else if (GET_PLAYER_FORM != PLAYER_FORM_HUMAN) {` | C | Maestro de esgrima: rechaza formas no humanas. |
| 118 | `src/overlays/actors/ovl_En_Kendo_Js/z_en_kendo_js.c` | 206 | `switch (GET_PLAYER_FORM) {` | C | Maestro de esgrima: texto por forma. |
| 119 | `src/overlays/actors/ovl_En_Kgy/z_en_kgy.c` | 390 | `if ((GET_PLAYER_FORM != PLAYER_FORM_HUMAN) \|\|` | C | Herrero (Gabora): requisito Humano + espada. |
| 120 | `src/overlays/actors/ovl_En_Lift_Nuts/z_en_lift_nuts.c` | 276 | `if ((EnLiftNuts_GetNumDaysWon() == 3) && (GET_PLAYER_FORM == PLAYER_FORM_DEKU) &&` | C | Deku del minijuego de saltos: se esconde si ganaste 3 días y eres Deku. |
| 121 | `src/overlays/actors/ovl_En_Lift_Nuts/z_en_lift_nuts.c` | 400 | `if (GET_PLAYER_FORM == PLAYER_FORM_DEKU) {` | C | Deku del minijuego: sólo Deku puede jugar. |
| 122 | `src/overlays/actors/ovl_En_Ma4/z_en_ma4.c` | 638 | `if ((GET_PLAYER_FORM != PLAYER_FORM_HUMAN) \|\| !CHECK_QUEST_ITEM(QUEST_SONG_EPONA)) {` | C | Romani: diálogo según forma. |
| 123 | `src/overlays/actors/ovl_En_Ma4/z_en_ma4.c` | 914 | `if (GET_PLAYER_FORM != PLAYER_FORM_HUMAN) {` | C | Romani: diálogo según forma. |
| 124 | `src/overlays/actors/ovl_En_Ma4/z_en_ma4.c` | 970 | `if (GET_PLAYER_FORM != PLAYER_FORM_HUMAN) {` | C | Romani: diálogo según forma. |
| 125 | `src/overlays/actors/ovl_En_Ma_Yto/z_en_ma_yto.c` | 1147 | `if ((Player_GetMask(play) != PLAYER_MASK_NONE) && (GET_PLAYER_FORM == PLAYER_FORM_HUMAN)) {` | C | Cremia: reacción a máscara si Humano. |
| 126 | `src/overlays/actors/ovl_En_Ma_Yto/z_en_ma_yto.c` | 1206 | `if ((Player_GetMask(play) != PLAYER_MASK_NONE) && (GET_PLAYER_FORM == PLAYER_FORM_HUMAN)) {` | C | Cremia: reacción a máscara si Humano. |
| 127 | `src/overlays/actors/ovl_En_Ma_Yts/z_en_ma_yts.c` | 332 | `if (GET_PLAYER_FORM != PLAYER_FORM_HUMAN) {` | C | Romani (niña): diálogo según forma. |
| 128 | `src/overlays/actors/ovl_En_Mk/z_en_mk.c` | 163 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Científico marino: diálogo/cutscene según forma (Zora). |
| 129 | `src/overlays/actors/ovl_En_Mk/z_en_mk.c` | 228 | `switch (GET_PLAYER_FORM) {` | C | Científico marino: diálogo/cutscene según forma (Zora). |
| 130 | `src/overlays/actors/ovl_En_Mk/z_en_mk.c` | 430 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Científico marino: diálogo/cutscene según forma (Zora). |
| 131 | `src/overlays/actors/ovl_En_Mm3/z_en_mm3.c` | 486 | `switch (GET_PLAYER_FORM) {` | C | Encargado de la oficina de correos (contador): texto según forma. |
| 132 | `src/overlays/actors/ovl_En_Mm3/z_en_mm3.c` | 524 | `switch (GET_PLAYER_FORM) {` | C | Encargado de la oficina de correos (contador): texto según forma. |
| 133 | `src/overlays/actors/ovl_En_Mnk/z_en_mnk.c` | 1427 | `if ((gSaveContext.save.playerForm == PLAYER_FORM_FIERCE_DEITY) \|\|` | C | Mono atado (Deku Palace): diálogo/acción según forma. |
| 134 | `src/overlays/actors/ovl_En_Mnk/z_en_mnk.c` | 1428 | `(gSaveContext.save.playerForm == PLAYER_FORM_HUMAN)) {` | C | Mono atado (Deku Palace): diálogo/acción según forma. |
| 135 | `src/overlays/actors/ovl_En_Mnk/z_en_mnk.c` | 1469 | `switch (gSaveContext.save.playerForm) {` | C | Mono atado (Deku Palace): diálogo/acción según forma. |
| 136 | `src/overlays/actors/ovl_En_Mnk/z_en_mnk.c` | 1533 | `if ((gSaveContext.save.playerForm == PLAYER_FORM_FIERCE_DEITY) \|\|` | C | Mono atado (Deku Palace): diálogo/acción según forma. |
| 137 | `src/overlays/actors/ovl_En_Mnk/z_en_mnk.c` | 1534 | `(gSaveContext.save.playerForm == PLAYER_FORM_HUMAN)) {` | C | Mono atado (Deku Palace): diálogo/acción según forma. |
| 138 | `src/overlays/actors/ovl_En_Mnk/z_en_mnk.c` | 1547 | `if ((gSaveContext.save.playerForm != PLAYER_FORM_FIERCE_DEITY) &&` | C | Mono atado (Deku Palace): diálogo/acción según forma. |
| 139 | `src/overlays/actors/ovl_En_Mnk/z_en_mnk.c` | 1548 | `(gSaveContext.save.playerForm != PLAYER_FORM_HUMAN)) {` | C | Mono atado (Deku Palace): diálogo/acción según forma. |
| 140 | `src/overlays/actors/ovl_En_Mnk/z_en_mnk.c` | 1584 | `if (gSaveContext.save.playerForm == PLAYER_FORM_DEKU) {` | C | Mono atado (Deku Palace): diálogo/acción según forma. |
| 141 | `src/overlays/actors/ovl_En_Mnk/z_en_mnk.c` | 1595 | `if (gSaveContext.save.playerForm == PLAYER_FORM_DEKU) {` | C | Mono atado (Deku Palace): diálogo/acción según forma. |
| 142 | `src/overlays/actors/ovl_En_Pamera/z_en_pamera.c` | 623 | `} else if ((GET_PLAYER_FORM != PLAYER_FORM_HUMAN) \|\|` | C | Pamela: diálogo según forma. |
| 143 | `src/overlays/actors/ovl_En_Pr/z_en_pr.c` | 368 | `this->unk_2B8 = D_80A338C0[GET_PLAYER_FORM] + player->actor.world.pos.y;` | C | Pez enemigo: altura objetivo según forma de `player`. |
| 144 | `src/overlays/actors/ovl_En_Pr/z_en_pr.c` | 405 | `this->unk_2B8 = D_80A338C0[GET_PLAYER_FORM] + player->actor.world.pos.y;` | C | Pez enemigo: altura objetivo según forma de `player`. |
| 145 | `src/overlays/actors/ovl_En_Prz/z_en_prz.c` | 371 | `(player->actor.world.pos.y + D_80A771E0[GET_PLAYER_FORM]) + Rand_CenteredFloat((2.0f * this-...` | C | Pez enemigo: altura objetivo según forma de `player`. |
| 146 | `src/overlays/actors/ovl_En_Railgibud/z_en_railgibud.c` | 385 | `if ((GET_PLAYER_FORM == PLAYER_FORM_GORON) \|\| (GET_PLAYER_FORM == PLAYER_FORM_DEKU)) {` | C | Gibdo: no puede agarrar a Goron/Deku. |
| 147 | `src/overlays/actors/ovl_En_Railgibud/z_en_railgibud.c` | 557 | `if ((GET_PLAYER_FORM != PLAYER_FORM_GORON) && (GET_PLAYER_FORM != PLAYER_FORM_DEKU) &&` | C | Gibdo: condición de agarre según forma. |
| 148 | `src/overlays/actors/ovl_En_Railgibud/z_en_railgibud.c` | 874 | `if (GET_PLAYER_FORM == PLAYER_FORM_HUMAN) {` | C | Gibdo: offset Y del agarre según forma. |
| 149 | `src/overlays/actors/ovl_En_Rr/z_en_rr.c` | 309 | `if (((this->unk_1E2 == 0) && (GET_PLAYER_FORM == PLAYER_FORM_HUMAN)) &&` | C | Like Like: roba el escudo sólo a Humano. |
| 150 | `src/overlays/actors/ovl_En_Syateki_Man/z_en_syateki_man.c` | 519 | `switch (GET_PLAYER_FORM) {` | C | Galería de tiro: texto intro según forma. |
| 151 | `src/overlays/actors/ovl_En_Syateki_Man/z_en_syateki_man.c` | 1221 | `if (GET_PLAYER_FORM == PLAYER_FORM_FIERCE_DEITY) {` | C | Galería de tiro: posición a la que se mueve al jugador si FD. |
| 152 | `src/overlays/actors/ovl_En_Takaraya/z_en_takaraya.c` | 184 | `this->actor.textId = D_80ADFB2C[GET_PLAYER_FORM];` | C | Cofres del tesoro: textId por forma. |
| 153 | `src/overlays/actors/ovl_En_Takaraya/z_en_takaraya.c` | 186 | `this->formSwitchFlag = GET_PLAYER_FORM + this->switchFlag;` | C | Cofres del tesoro: switch flag por forma (premio distinto por forma). |
| 154 | `src/overlays/actors/ovl_En_Takaraya/z_en_takaraya.c` | 195 | `getItemId = D_80ADFB38[GET_PLAYER_FORM][1];` | C | Cofres del tesoro: getItemId por forma. |
| 155 | `src/overlays/actors/ovl_En_Takaraya/z_en_takaraya.c` | 197 | `getItemId = D_80ADFB38[GET_PLAYER_FORM][0];` | C | Cofres del tesoro: getItemId por forma. |
| 156 | `src/overlays/actors/ovl_En_Takaraya/z_en_takaraya.c` | 254 | `this->actor.textId = D_80ADFB44[GET_PLAYER_FORM];` | C | Cofres del tesoro: textId por forma. |
| 157 | `src/overlays/actors/ovl_En_Takaraya/z_en_takaraya.c` | 361 | `this->formSwitchFlag = GET_PLAYER_FORM + this->switchFlag;` | C | Cofres del tesoro: switch flag por forma. |
| 158 | `src/overlays/actors/ovl_En_Takaraya/z_en_takaraya.c` | 385 | `this->actor.textId = D_80ADFB50[GET_PLAYER_FORM];` | C | Cofres del tesoro: textId por forma. |
| 159 | `src/overlays/actors/ovl_En_Takaraya/z_en_takaraya.c` | 427 | `if ((GET_PLAYER_FORM == PLAYER_FORM_DEKU) \|\| (GET_PLAYER_FORM == PLAYER_FORM_HUMAN)) {` | C | EnTakaraya_Draw (draw del NPC, no del jugador): mira hacia abajo si Deku/Humano. |
| 160 | `src/overlays/actors/ovl_En_Talk_Gibud/z_en_talk_gibud.c` | 349 | `if ((GET_PLAYER_FORM == PLAYER_FORM_GORON) \|\| (GET_PLAYER_FORM == PLAYER_FORM_DEKU)) {` | C | Gibdo (que habla): no puede agarrar a Goron/Deku. |
| 161 | `src/overlays/actors/ovl_En_Talk_Gibud/z_en_talk_gibud.c` | 516 | `if ((GET_PLAYER_FORM != PLAYER_FORM_GORON) && (GET_PLAYER_FORM != PLAYER_FORM_DEKU) &&` | C | Gibdo (que habla): condición de agarre. |
| 162 | `src/overlays/actors/ovl_En_Talk_Gibud/z_en_talk_gibud.c` | 1005 | `if (GET_PLAYER_FORM == PLAYER_FORM_HUMAN) {` | C | Gibdo (que habla): offset Y del agarre. |
| 163 | `src/overlays/actors/ovl_En_Thiefbird/z_en_thiefbird.c` | 237 | `if (GET_PLAYER_FORM == PLAYER_FORM_HUMAN) {` | C | Takkuri: qué roba según forma (espada si Humano). |
| 164 | `src/overlays/actors/ovl_En_Time_Tag/z_en_time_tag.c` | 260 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Diario de Lulu/Mikau: legible sólo como Zora. |
| 165 | `src/overlays/actors/ovl_En_Toto/z_en_toto.c` | 428 | `temp_v1_2 = &D_80BA50DC[gSaveContext.save.playerForm - 1];` | C | Toto: texto por forma. |
| 166 | `src/overlays/actors/ovl_En_Toto/z_en_toto.c` | 504 | `temp_s0 = &D_80BA50DC[gSaveContext.save.playerForm - 1];` | C | Toto: posición de escenario por forma; ya usa `player`. |
| 167 | `src/overlays/actors/ovl_En_Toto/z_en_toto.c` | 517 | `Message_StartTextbox(play, D_80BA50DC[GET_PLAYER_FORM - 1].unk2, NULL);` | C | Toto (Milk Bar): texto por forma en la Balada. |
| 168 | `src/overlays/actors/ovl_En_Toto/z_en_toto.c` | 538 | `if (GET_PLAYER_FORM == PLAYER_FORM_HUMAN) {` | C | Toto: marca weekEventReg de la forma que tocó en la Balada (check rando VB_BALLAD_PLAYED_FORM). |
| 169 | `src/overlays/actors/ovl_En_Toto/z_en_toto.c` | 541 | `if (GET_PLAYER_FORM == PLAYER_FORM_DEKU) {` | C | Toto: marca weekEventReg de la forma que tocó en la Balada (check rando VB_BALLAD_PLAYED_FORM). |
| 170 | `src/overlays/actors/ovl_En_Toto/z_en_toto.c` | 544 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Toto: marca weekEventReg de la forma que tocó en la Balada (check rando VB_BALLAD_PLAYED_FORM). |
| 171 | `src/overlays/actors/ovl_En_Toto/z_en_toto.c` | 547 | `if (GET_PLAYER_FORM == PLAYER_FORM_GORON) {` | C | Toto: marca weekEventReg de la forma que tocó en la Balada (check rando VB_BALLAD_PLAYED_FORM). |
| 172 | `src/overlays/actors/ovl_En_Toto/z_en_toto.c` | 573 | `if ((GET_PLAYER_FORM != (i + 1)) && (D_80BA5128[i] & this->unk2B3)) {` | C | Toto: spawnea los 'fantasmas' de las otras formas. |
| 173 | `src/overlays/actors/ovl_En_Tsn/z_en_tsn.c` | 166 | `} else if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Pescador (Great Bay): diálogo Zora. |
| 174 | `src/overlays/actors/ovl_En_Wallmas/z_en_wallmas.c` | 483 | `player->actor.world.pos.y = this->actor.world.pos.y - sYOffsetPerForm[GET_PLAYER_FORM];` | C | Wallmaster: altura a la que sujeta a `player` según forma. |
| 175 | `src/overlays/actors/ovl_En_Wallmas/z_en_wallmas.c` | 494 | `Math_StepToF(&this->actor.world.pos.y, sYOffsetPerForm[GET_PLAYER_FORM] + player->actor.worl...` | C | Wallmaster: altura según forma de `player`. |
| 176 | `src/overlays/actors/ovl_En_Yb/z_en_yb.c` | 363 | `(GET_PLAYER_FORM == PLAYER_FORM_HUMAN)) {` | C | Kamaro: Canción de Curación tocada por Humano. |
| 177 | `src/overlays/actors/ovl_En_Zob/z_en_zob.c` | 230 | `if (GET_PLAYER_FORM != PLAYER_FORM_ZORA) {` | C | Japas (banda Zora): diálogo Zora. |
| 178 | `src/overlays/actors/ovl_En_Zob/z_en_zob.c` | 583 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Japas (banda Zora): diálogo Zora. |
| 179 | `src/overlays/actors/ovl_En_Zob/z_en_zob.c` | 614 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Japas (banda Zora): diálogo Zora. |
| 180 | `src/overlays/actors/ovl_En_Zod/z_en_zod.c` | 157 | `if (GET_PLAYER_FORM != PLAYER_FORM_ZORA) {` | C | Tijo (banda Zora): diálogo Zora. |
| 181 | `src/overlays/actors/ovl_En_Zod/z_en_zod.c` | 399 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Tijo (banda Zora): diálogo Zora. |
| 182 | `src/overlays/actors/ovl_En_Zog/z_en_zog.c` | 663 | `if ((play->msgCtx.lastPlayedSong == OCARINA_SONG_HEALING) && (GET_PLAYER_FORM == PLAYER_FORM...` | C | Mikau: Canción de Curación tocada por Humano. |
| 183 | `src/overlays/actors/ovl_En_Zog/z_en_zog.c` | 751 | `if ((play->msgCtx.lastPlayedSong == OCARINA_SONG_HEALING) && (GET_PLAYER_FORM == PLAYER_FORM...` | C | Mikau: Canción de Curación tocada por Humano. |
| 184 | `src/overlays/actors/ovl_En_Zos/z_en_zos.c` | 215 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Evan (banda Zora): diálogo Zora. |
| 185 | `src/overlays/actors/ovl_En_Zos/z_en_zos.c` | 500 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Evan (banda Zora): diálogo Zora. |
| 186 | `src/overlays/actors/ovl_En_Zos/z_en_zos.c` | 588 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Evan (banda Zora): diálogo Zora. |
| 187 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 386 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 188 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 437 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 189 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 452 | `} else if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 190 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 590 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 191 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 595 | `} else if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 192 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 612 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 193 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 735 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 194 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 834 | `if ((this->actor.xzDistToPlayer < 120.0f) && (GET_PLAYER_FORM == PLAYER_FORM_ZORA)) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 195 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 864 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 196 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 882 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 197 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 911 | `if (GET_PLAYER_FORM != PLAYER_FORM_ZORA) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 198 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 918 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 199 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 1106 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 200 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 1184 | `if ((GET_PLAYER_FORM == PLAYER_FORM_ZORA) \|\| (this->actor.xzDistToPlayer < 100.0f)) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 201 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 1197 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 202 | `src/overlays/actors/ovl_En_Zot/z_en_zot.c` | 1284 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zoras de Zora Hall: diálogo/interacción Zora. |
| 203 | `src/overlays/actors/ovl_En_Zov/z_en_zov.c` | 205 | `if (GET_PLAYER_FORM != PLAYER_FORM_ZORA) {` | C | Lulu: diálogo/animación Zora. |
| 204 | `src/overlays/actors/ovl_En_Zov/z_en_zov.c` | 219 | `} else if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Lulu: diálogo/animación Zora. |
| 205 | `src/overlays/actors/ovl_En_Zov/z_en_zov.c` | 409 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Lulu: diálogo/animación Zora. |
| 206 | `src/overlays/actors/ovl_En_Zow/z_en_zow.c` | 374 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zora (playa): diálogo Zora. |
| 207 | `src/overlays/actors/ovl_En_Zow/z_en_zow.c` | 387 | `} else if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zora (playa): diálogo Zora. |
| 208 | `src/overlays/actors/ovl_En_Zow/z_en_zow.c` | 401 | `if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zora (playa): diálogo Zora. |
| 209 | `src/overlays/actors/ovl_En_Zow/z_en_zow.c` | 406 | `} else if (GET_PLAYER_FORM == PLAYER_FORM_ZORA) {` | C | Zora (playa): diálogo Zora. |
| 210 | `src/overlays/actors/ovl_Obj_Um/z_obj_um.c` | 912 | `if (GET_PLAYER_FORM == PLAYER_FORM_HUMAN) {` | C | Cremia (carreta): diálogo según forma. |
| 211 | `src/overlays/actors/ovl_Oceff_Spot/z_oceff_spot.c` | 119 | `switch (GET_PLAYER_FORM) {` | C (?) | Efecto visual de canción: escala según forma. Debería usar la forma del Player que toca (quien spawnea el efecto); usa GET_PLAYER(play) para la posición. |
| 212 | `src/overlays/actors/ovl_Oceff_Storm/z_oceff_storm.c` | 102 | `switch (GET_PLAYER_FORM) {` | C (?) | Efecto de la Canción de la Tormenta: escala según forma; igual que Oceff_Spot. |
| 213 | `src/overlays/actors/ovl_player_actor/z_player.c` | 6157 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | B · W | func_808345C8: al reaparecer/morir vuelve a Humano si tiene máscara Deku. |
| 214 | `src/overlays/actors/ovl_player_actor/z_player.c` | 7784 | `gSaveContext.save.playerForm = playerForm;` | B · W | func_808388B8: fija la forma DESTINO al empezar la transformación con máscara (this->transformation sigue siendo la vieja hasta la recarga en func_8012301C). |
| 215 | `src/overlays/actors/ovl_player_actor/z_player.c` | 11296 | `this->transformation = GET_PLAYER_FORM;` | B (?) | Player_Init: inicializa this->transformation desde el save (rama rot.x==0). Un puppet spawneado así tomaría la forma del jugador LOCAL. Ver caveats. |
| 216 | `src/overlays/actors/ovl_player_actor/z_player.c` | 14133 | `(GET_PLAYER_FORM << 3) \| this->transformation);` | B | func_80848640 (Elegía del Vacío): params de EffChange = (formaSave<<3)\|this->transformation; fuera de una transformación ambos coinciden, usar this->transformation. |
| 217 | `src/overlays/actors/ovl_player_actor/z_player.c` | 19120 | `SET_WEEKEVENTREG(D_8085D908[GET_PLAYER_FORM]);` | B (?) | Player_Action_86 (ponerse/quitarse máscara de transformación): SET_WEEKEVENTREG del primer uso de la forma DESTINO (efecto en save compartido). |
| 218 | `src/overlays/actors/ovl_player_actor/z_player.c` | 19125 | `((this->transformation != PLAYER_FORM_HUMAN) \|\| CHECK_WEEKEVENTREG(D_8085D908[GET_PLAYER_FOR...` | B (?) | Player_Action_86: permite saltar la animación si ya se usó la forma destino. Requiere forma destino por-Player. |
| 219 | `src/overlays/actors/ovl_player_actor/z_player.c` | 20969 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | B · W | Player_CsAction_6: cutscene fuerza Humano y recarga vía func_8012301C. |
| 220 | `src/overlays/actors/ovl_player_actor/z_player.c` | 21137 | `gSaveContext.save.playerForm = PLAYER_FORM_DEKU;` | B · W | Player_CsAction_22: cutscene fuerza Deku. |
| 221 | `src/overlays/actors/ovl_player_actor/z_player.c` | 21143 | `if (GET_PLAYER_FORM != this->transformation) {` | B (?) | Player_CsAction_23: si formaSave != this->transformation, recarga vía func_8012301C (transformación forzada por cutscene). |
| 222 | `src/overlays/actors/ovl_player_actor/z_player.c` | 21167 | `gSaveContext.save.playerForm = PLAYER_FORM_FIERCE_DEITY;` | B · W | Player_CsAction_25: cutscene fuerza Fierce Deity. |
| 223 | `src/overlays/actors/ovl_player_actor/z_player.c` | 21173 | `if (GET_PLAYER_FORM != this->transformation) {` | B (?) | Player_CsAction_26: idem para Fierce Deity. |
| 224 | `src/overlays/actors/ovl_player_actor/z_player.c` | 21333 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | B · W | Player_CsAction_43: cutscene fuerza Humano y recarga. |
| 225 | `src/overlays/gamestates/ovl_opening/z_opening.c` | 37 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | A · W | Pantalla de título: Humano. |
| 226 | `src/overlays/gamestates/ovl_select/z_select.c` | 570 | `playerForm = GET_PLAYER_FORM - 1;` | A | Map Select: ciclar forma con B (debug). |
| 227 | `src/overlays/gamestates/ovl_select/z_select.c` | 574 | `gSaveContext.save.playerForm = playerForm;` | A · W | Map Select: aplicar forma elegida (debug). |
| 228 | `src/overlays/gamestates/ovl_select/z_select.c` | 1041 | `MapSelect_PrintAgeSetting(this, &printer, GET_PLAYER_FORM);` | A | Map Select: imprime la forma (debug). |
| 229 | `src/overlays/gamestates/ovl_select/z_select.c` | 1142 | `gSaveContext.save.playerForm = PLAYER_FORM_HUMAN;` | A · W | Map Select init: Humano. |
| 230 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_item.c` | 206 | `if (!gPlayerFormSlotRestrictions[GET_PLAYER_FORM][SLOT(item)]) {` | A | Menú de pausa: ítems restringidos (gris) según forma. |
| 231 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_item.c` | 306 | `(pauseCtx->cursorSpecialPos == 0) && gPlayerFormSlotRestrictions[GET_PLAYER_FORM][i]) {` | A | Menú de pausa: ítems restringidos (gris) según forma. |
| 232 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_item.c` | 338 | `VB_ITEM_BE_RESTRICTED, !gPlayerFormItemRestrictions[GET_PLAYER_FORM][(s32)itemId], &itemId);` | A | Menú de pausa: ítems restringidos (gris) según forma. |
| 233 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_item.c` | 643 | `if (GET_PLAYER_FORM != PLAYER_FORM_HUMAN) {` | A | Menú de pausa: impedir desequipar la máscara de la forma actual. |
| 234 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_item.c` | 646 | `if (sPlayerFormItems[GET_PLAYER_FORM] == BUTTON_ITEM_EQUIP(0, EQUIP_SLOT_C_LEFT)) {` | A | Menú de pausa: impedir desequipar la máscara de la forma actual. |
| 235 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_item.c` | 651 | `if (sPlayerFormItems[GET_PLAYER_FORM] == BUTTON_ITEM_EQUIP(0, EQUIP_SLOT_C_DOWN)) {` | A | Menú de pausa: impedir desequipar la máscara de la forma actual. |
| 236 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_item.c` | 656 | `if (sPlayerFormItems[GET_PLAYER_FORM] == BUTTON_ITEM_EQUIP(0, EQUIP_SLOT_C_RIGHT)) {` | A | Menú de pausa: impedir desequipar la máscara de la forma actual. |
| 237 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_item.c` | 664 | `if (sPlayerFormItems[GET_PLAYER_FORM] ==` | A | Menú de pausa: impedir desequipar la máscara de la forma actual. |
| 238 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_item.c` | 670 | `if (sPlayerFormItems[GET_PLAYER_FORM] == DPAD_BUTTON_ITEM_EQUIP(0, EQUIP_SLOT_D_LEFT)) {` | A | Menú de pausa: impedir desequipar la máscara de la forma actual. |
| 239 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_item.c` | 675 | `if (sPlayerFormItems[GET_PLAYER_FORM] == DPAD_BUTTON_ITEM_EQUIP(0, EQUIP_SLOT_D_DOWN)) {` | A | Menú de pausa: impedir desequipar la máscara de la forma actual. |
| 240 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_item.c` | 680 | `if (sPlayerFormItems[GET_PLAYER_FORM] == DPAD_BUTTON_ITEM_EQUIP(0, EQUIP_SLOT_D_UP)) {` | A | Menú de pausa: impedir desequipar la máscara de la forma actual. |
| 241 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_mask.c` | 248 | `(pauseCtx->cursorSpecialPos == 0) && gMaskPlayerFormSlotRestrictions[GET_PLAYER_FORM][i]) {` | A | Menú de máscaras: slots restringidos según forma. |
| 242 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_mask.c` | 570 | `((sMaskPlayerFormItems[GET_PLAYER_FORM] != ITEM_NONE) &&` | A | Menú de máscaras: impedir reasignar la máscara de la forma actual. |
| 243 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_mask.c` | 571 | `(sMaskPlayerFormItems[GET_PLAYER_FORM] == BUTTON_ITEM_EQUIP(0, EQUIP_SLOT_C_LEFT)))) {` | A | Menú de máscaras: impedir reasignar la máscara de la forma actual. |
| 244 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_mask.c` | 578 | `((sMaskPlayerFormItems[GET_PLAYER_FORM] != ITEM_NONE) &&` | A | Menú de máscaras: impedir reasignar la máscara de la forma actual. |
| 245 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_mask.c` | 579 | `(sMaskPlayerFormItems[GET_PLAYER_FORM] == BUTTON_ITEM_EQUIP(0, EQUIP_SLOT_C_DOWN)))) {` | A | Menú de máscaras: impedir reasignar la máscara de la forma actual. |
| 246 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_mask.c` | 586 | `((sMaskPlayerFormItems[GET_PLAYER_FORM] != ITEM_NONE) &&` | A | Menú de máscaras: impedir reasignar la máscara de la forma actual. |
| 247 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_mask.c` | 587 | `(sMaskPlayerFormItems[GET_PLAYER_FORM] == BUTTON_ITEM_EQUIP(0, EQUIP_SLOT_C_RIGHT)))) {` | A | Menú de máscaras: impedir reasignar la máscara de la forma actual. |
| 248 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_mask.c` | 597 | `((sMaskPlayerFormItems[GET_PLAYER_FORM] != ITEM_NONE) &&` | A | Menú de máscaras: impedir reasignar la máscara de la forma actual. |
| 249 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_mask.c` | 598 | `(sMaskPlayerFormItems[GET_PLAYER_FORM] ==` | A | Menú de máscaras: impedir reasignar la máscara de la forma actual. |
| 250 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_mask.c` | 606 | `((sMaskPlayerFormItems[GET_PLAYER_FORM] != ITEM_NONE) &&` | A | Menú de máscaras: impedir reasignar la máscara de la forma actual. |
| 251 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_mask.c` | 607 | `(sMaskPlayerFormItems[GET_PLAYER_FORM] ==` | A | Menú de máscaras: impedir reasignar la máscara de la forma actual. |
| 252 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_mask.c` | 615 | `((sMaskPlayerFormItems[GET_PLAYER_FORM] != ITEM_NONE) &&` | A | Menú de máscaras: impedir reasignar la máscara de la forma actual. |
| 253 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_mask.c` | 616 | `(sMaskPlayerFormItems[GET_PLAYER_FORM] ==` | A | Menú de máscaras: impedir reasignar la máscara de la forma actual. |
| 254 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_mask.c` | 624 | `((sMaskPlayerFormItems[GET_PLAYER_FORM] != ITEM_NONE) &&` | A | Menú de máscaras: impedir reasignar la máscara de la forma actual. |
| 255 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_mask.c` | 625 | `(sMaskPlayerFormItems[GET_PLAYER_FORM] ==` | A | Menú de máscaras: impedir reasignar la máscara de la forma actual. |
| 256 | `src/overlays/kaleido_scope/ovl_kaleido_scope/z_kaleido_scope_NES.c` | 3371 | `if (!gPlayerFormItemRestrictions[GET_PLAYER_FORM][(s32)itemId]) {` | A | KaleidoScope_Update: ítems restringidos según forma. |

## Propuesta: `PLAYER_FORM_OF(player)`

### Macro

```c
// include/z64player.h (o un header del coop)
// Forma ACTUAL de un Player concreto (local o puppet).
#define PLAYER_FORM_OF(player) ((PlayerTransformation)((player)->transformation))
```

`Player::transformation` es `u8` (`include/z64player.h:1144`), igual que `SaveContext::save.playerForm` (`include/z64save.h:425`), así que el cambio es transparente en comparaciones, `switch` e indexado de tablas (`sTable[PLAYER_FORM_OF(player)]`).

Para el jugador local fuera de la ventana de transformación se cumple `PLAYER_FORM_OF(GET_PLAYER(play)) == GET_PLAYER_FORM`, de modo que sustituir un uso de categoría C por `PLAYER_FORM_OF(GET_PLAYER(play))` no cambia el comportamiento en un jugador.

### Pautas por categoría

- **A (UI / save local)**: se queda en `GET_PLAYER_FORM`. Describe al jugador local (HUD, kaleido, botones, save editor, map select, carga/guardado, lógica del rando). No tiene sentido mirar la forma del puppet aquí.
- **B (lógica de Link)**: usar `this->transformation` / `player->transformation` directamente (o `PLAYER_FORM_OF(this)`). **Excepción**: los usos que leen la forma *destino* de una transformación (`z_player_lib.c:407/417`, `z_player.c:11296/19120/19125/21143/21173`, `FastTransformation.cpp:38/51/59`) no pueden usar `transformation` porque en ese momento todavía vale la forma vieja. Para ellos se necesita un campo por-Player de "forma pendiente", p. ej. `PLAYER_TARGET_FORM_OF(player)` que para el local devuelva `GET_PLAYER_FORM` y para el puppet devuelva la forma destino recibida por red.
- **C (mundo)**: el call site debe obtener el `Player*` **que interactúa** y usar `PLAYER_FORM_OF(thatPlayer)`. Fuentes habituales:
  - Diálogo: el jugador que aceptó la conversación (`Actor_TalkOfferAccepted` hoy asume `GET_PLAYER(play)`; en coop hará falta un helper tipo `Coop_GetTalkingPlayer(play, actor)` o comprobar `player->talkActor == actor`).
  - Enemigos que agarran/persiguen (`En_Railgibud`, `En_Talk_Gibud`, `En_Wallmas`, `En_Rr`, `En_Pr`, `En_Prz`, `En_Dinofos`, `En_Floormas`, `En_Thiefbird`): su objetivo. La mayoría ya tiene una variable local `player`; basta con que esa variable pase a ser el objetivo real y usar `PLAYER_FORM_OF(player)`.
  - Plataformas (`Bg_Danpei_Movebg`, `Bg_Lotus`): el jugador que está encima (`DynaPolyActor_IsPlayerOnTop` hoy sólo mira al local).
  - Canciones de ocarina (`En_Gs`, `En_Hg`, `En_Yb`, `En_Zog`, `En_Toto`, `En_Mnk`, `Oceff_*`): el jugador que tocó la canción; si la canción llega por red, el evento debe incluir la forma del que tocó.
  - Cofres / ítems (`En_Box`, `Demo_Tre_Lgt`, `En_Takaraya`, `CustomItem`, `z_en_item00`): el jugador que abre/recoge/causa el drop.
  - Puertas (`FierceDeityAnywhere` → `VB_BE_NEAR_DOOR`): el jugador cercano a la puerta.
  - Fallback razonable cuando no hay "interactor" claro: el jugador más cercano al actor.
- **Escrituras**: sólo deben afectar al jugador local. El puppet nunca debe ejecutar rutas que escriban `gSaveContext.save.playerForm` (ver tabla de escrituras); su forma se actualiza desde la red escribiendo `puppet->transformation` (y recargando su objeto/esqueleto).

### Caveats

1. **Ventana `transformation` vs `playerForm` (transformación con máscara)**. `func_808388B8` (`z_player.c:7784`) escribe la forma **destino** en el save al empezar `Player_Action_86`; `this->transformation` conserva la forma vieja hasta que `func_8012301C` (`z_player_lib.c:400-424`, update temporal) recarga el objeto de la nueva forma y vuelve a llamar a `Player_Init`, que hace `this->transformation = GET_PLAYER_FORM` (`z_player.c:11296`). Durante esa ventana `GET_PLAYER_FORM != player->transformation`. Lo mismo pasa con las transformaciones forzadas por cutscene (`Player_CsAction_6/22/23/25/26/43`, `z_demo.c` `CS_MISC_PLAYER_FORM_*`). Consecuencias:
   - Un uso C migrado a `PLAYER_FORM_OF` verá la forma vieja durante la ventana, mientras que `GET_PLAYER_FORM` vería la nueva. En la práctica el jugador está en una acción/cutscene sin interacción, así que debería ser inocuo, pero es un cambio de comportamiento a tener en cuenta.
   - `z_parameter.c:3357` y `:9336` (HUD) comparan deliberadamente ambos valores para detectar la ventana: deben seguir usando la forma global del local.
   - El puppet necesita su propia "forma pendiente" para replicar la transformación (ver pautas B).
2. **`Player_Init` lee `GET_PLAYER_FORM`** (`z_player.c:11296`) en la rama `shape.rot.x == 0`. Un puppet creado con `Actor_Spawn(ACTOR_PLAYER, ...)` tomaría la forma del jugador local. Además esa rama **escribe** `gSaveContext.save.equippedMask` y llama a `Inventory_UpdateDeitySwordEquip` (`z_parameter.c:4860`, modifica el botón B del save local). La rama `shape.rot.x != 0` (`transformation = rot.x - 1`) no es un "spawn con forma" limpio: en `z_player.c:11402` pone `PLAYER_CSACTION_68` y retorna. Opciones: envolver el `Init` del puppet intercambiando temporalmente `playerForm`/`equippedMask`/equipos con los del puppet y restaurándolos después (mismo patrón que `CoopSnapshot` `CapturePerPlayer`/`RestorePerPlayer`), o añadir un hook que fije `transformation` desde el estado del puppet.
3. **Carga del objeto de forma** (`z_scene.c:182`, `z_scene_2SH.cpp:64`). Al cargar la escena sólo se carga como persistente `gPlayerFormObjectIds[GET_PLAYER_FORM]` (objeto del jugador local). Si el puppet tiene otra forma, su `OBJECT_LINK_*` no está cargado y `Player_Init`/el draw del puppet fallarían o usarían un objeto incorrecto. Además `gActorOverlayTable[ACTOR_PLAYER].profile->objectId` es **global** y lo reescriben `Player_Init` y `func_8012301C`: los dos Players comparten ese campo.
4. **`Player_GetStrength(void)`** (`z_player_lib.c:1677`) usa la forma global y no recibe `Player*`; se usa en `z_player.c:7414` y en `ovl_Obj_Oshihiki` (bloques empujables). Conviene una variante con `Player*`.
5. **Macros de botones por forma** (`CUR_FORM`, `CUR_FORM_EQUIP`, `GET_CUR_FORM_BTN_ITEM/SLOT`, `DPAD_*`; ~80 usos) leen `save.playerForm` indirectamente. No están en el alcance de esta tabla, pero el código de `Player` las usa para decidir qué ítem/máscara usar (`z_player.c:3776`, `3809`, `3875`, `3877`) y para el modelo de la vaina (`Player_SetModels`, `z_player_lib.c:1528`). Para el puppet esos valores deberían venir de sus propios equipos.
6. **Lógica del rando** (`Rando/Logic/Logic.h:178-182`): las macros `IS_*`/`CAN_BE_*` evalúan la alcanzabilidad sobre el save local. Se han marcado A (?): si en coop la lógica debe considerar a ambos jugadores, sería un rediseño aparte, no un cambio de macro.

## Usos imprescindibles para dibujar al puppet

El camino de dibujo de `Player` **no lee la forma global**:

- `Player_Draw` (`z_player.c:13279-13496`) → `Player_DrawGameplay` (`z_player.c:13177`) → `Player_DrawImpl(..., this->transformation, ...)` (`z_player_lib.c:2057`). `Player_DrawImpl` recibe la forma por parámetro (`playerForm`) y con ella elige texturas de ojos/boca (`sPlayerEyesTextures[playerForm]`, `sPlayerMouthTextures[playerForm]`) y `D_801F59E0 = playerForm * 2`.
- Callbacks de limbs (`Player_OverrideLimbDrawGameplayCommon/Default/FirstPerson/Crawling`, `Player_PostLimbDrawGameplay`) y los dibujos de máscaras/accesorios (`Player_DrawBlastMask`, `Player_DrawGreatFairysMask`, `Player_DrawZoraShield`, `Player_DrawGetItem`, ...) usan el `Player*`/`Actor*` que reciben.
- Verificación: en `z_player_lib.c` no hay ningún `gSaveContext`, `GET_PLAYER_FORM`, `CUR_FORM` ni `GET_PLAYER(` a partir de la línea 2000 (todo el bloque de draw), y en `Player_Draw`/`Player_DrawGameplay` (`z_player.c:13177-13496`) tampoco.
- El esqueleto se elige en `Player_InitCommon(this, play, gPlayerSkeletons[this->transformation])` (`z_player.c:11351`), también por `transformation`.

Por tanto **no hace falta reemplazar ningún uso de la forma global para dibujar al puppet**: basta con que `puppet->transformation` sea correcto y que el objeto `OBJECT_LINK_*` de esa forma esté cargado y asignado a `puppet->actor.objectSlot`.

Excepciones del camino de dibujo que leen la forma global: **ninguna directa** (ningún uso de `GET_PLAYER_FORM` ni `save.playerForm` en el draw). Dependencias indirectas a vigilar (no son lecturas de la forma en el draw):

- `Player_SetModels` (`z_player_lib.c:1528`, ejecutado en update, no en draw) usa `CUR_FORM_EQUIP(EQUIP_SLOT_B)` (equipos del save local, indexados por la forma global) para elegir la vaina vacía; puede dar un modelo de vaina incorrecto al puppet.
- La carga del objeto de forma (`z_scene.c:182`, `z_scene_2SH.cpp:64`, `func_8012301C`) y el `objectId` global de `gActorOverlayTable[ACTOR_PLAYER]` (caveat 3): sin el objeto correcto el draw del puppet no funciona aunque use `transformation`.
- `Player_DrawBunnyHood(PlayState*)` (`z_player_lib.c:3459`) no recibe `Player*` y usa el estado global `sBunnyEarKinematics`: no depende de la forma, pero ambos Links compartirían la física de las orejas.
