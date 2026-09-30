# FlashVita: piano Vita-native

Audit del 2026-09-27 su `main` @ `c92203f` e sul fork Ruffle `flashvita-v0.15.27` @ `0e30f1b`.
Integra `PERFORMANCE_AUDIT.md`, che resta la fonte delle misure su hardware.

Obiettivo: far usare a FlashVita CPU, GPU e memoria della console in modo nativo, invece di
far girare Ruffle come su un desktop lento. I livelli di impatto (Critico, Alto, Medio) sono
stime qualitative, non misure; ogni voce va validata con A/B su hardware.

## Sintesi

Le misure esistenti mostrano Pacman a circa 2,7 FPS prima della correzione del puntatore e
Cubefield oltre i 300 ms per ciclo, con 267,8 ms di GC in un solo tick. Dal codice emergono
quattro cause, in ordine di probabile impatto:

1. **Memoria.** La heap di Ruffle vive nel pool RAM di vitaGL, allocato come memoria
   non-cached. Sospetto forte, da confermare sulla console.
2. **Rendering.** La CPU trasforma ogni vertice a ogni frame (circa 811k ogni 30 frame su
   Cubefield), lo copia due volte e ridisegna anche senza un nuovo frame Flash.
3. **VM e GC.** Il GC usa il pacing di default con `collect_debt()` a ogni `update()`;
   l'AVM1 re-interpreta il bytecode appoggiandosi a cache hash.
4. **Core.** Logica, render e submit GPU sono seriali sul core 0; i due worker servono solo
   per trasformare vertici e fare predecode.

## Sospetto n. 1: heap di Ruffle in memoria non-cached

- `vglInitExtended` (`src/main.cpp:121`) viene chiamato senza `vglUseCachedMem(GL_TRUE)`.
  In vitaGL-fresh `has_cached_mem` sta in `.bss`, quindi vale false, e il pool `VGL_MEM_RAM`
  viene allocato come `SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE`.
- `flashvita_vita_rust_allocator_enable_vgl()` attiva subito lo spill mode
  (`src/platform/vita_native.cpp:1139`): da lì ogni allocazione Rust e ogni `malloc` passa
  da `vglAlloc`. Oggetti GC, display list, mesh e stringhe AVM finiscono in quel pool.
- Nel log v0.15.26 il pool RAM di vitaGL è di 139 MB, mentre la heap newlib da 192 MB
  (`src/main.cpp:27`, cached) resta quasi inutilizzata da Ruffle.
- Se il pool si esaurisce, il fallback va in `VGL_MEM_PHYCONT` (anch'esso non-cached) e poi
  in VRAM (CDRAM), dove le letture CPU sono ancora più lente.
- Sul Cortex-A9 ogni load da memoria uncached va in DRAM, e il GC passa il tempo proprio a
  rileggere la heap.

**Verifica (circa mezz'ora):** loggare `sceKernelGetMemBlockInfoByAddr(ptr).type` su
un'allocazione Rust fatta a player avviato.
**Fix rapido:** `vglUseCachedMem(GL_TRUE)` prima di `vglInitExtended`.
**Fix corretto:** allocatore dedicato su memoria cached (R4).

## Priorità 0: correzioni rapide (giorni)

| ID | Problema | Dove | Impatto |
| --- | --- | --- | --- |
| P0-1 | Heap Ruffle in memoria non-cached (sezione precedente) | `src/main.cpp:121`, `src/platform/vita_native.cpp:1139` | Critico, da verificare |
| P0-2 | Il realloc Rust copia sempre | `src/platform/vita_native.cpp:1193` | Alto |
| P0-3 | Render anche senza un nuovo frame Flash | `src/player/ruffle_runtime.cpp:663` | Alto |
| P0-4 | Upload completi e riallocazioni delle texture dinamiche | `rust/ruffle_bridge/src/vita_renderer.rs:1583`, `src/player/vitagl_bridge.cpp:215` | Medio |
| P0-5 | AVM2 sempre in `PlayerMode::Debug` | `rust/ruffle_bridge/src/lib.rs:564` | Medio |
| P0-6 | Log attivi di default, un open/close per riga | `src/config/app_config.h:9`, `src/platform/vita_native.cpp:589` | Medio |
| P0-7 | Flag di build e contatori di profiling sempre attivi | `rust/ruffle_bridge/Cargo.toml:21` | Medio |
| P0-8 | Audio con la stessa priorità dei worker | `src/platform/vita_native.cpp:401`, `:841` | Medio |

**P0-2.** `flashvita_vita_rust_realloc` fa sempre alloc + memcpy + free, anche se esiste
`sceClibMspaceRealloc`: ogni `Vec` o `String` che cresce paga una copia completa. Restano
anche un `mallinfo()` ogni 64 allocazioni e una syscall `sceKernelGetMemBlockInfoByAddr`
nella `free` per i puntatori fuori dai range noti.

**P0-3.** `RuffleRuntime::tick` chiama `headless_render` a ogni giro di loop, anche quando
`Player::tick` non ha eseguito nessun `run_frame`. Con uno SWF a 24 fps e vsync a 60 Hz sono
circa 2,5 render su 3 sprecati, quando il gioco sta nei tempi. Proposta:

- renderizzare solo se è girato almeno un frame o se è cambiato qualcosa (`needs_render`);
- altrimenti niente swap: chiamare `sceDisplayWaitVblankStart()` per mantenere la cadenza.

La regressione di pacing vista in passato con il gating (citata in `PORTING_STATUS.md`) va
ricontrollata con questa variante; la sua causa non è documentata.

**P0-4.** `update_texture` converte l'intera bitmap con `to_rgba()` anche per regioni
piccole, e il ramo completo usa `glTexImage2D`, che in vitaGL rialloca lo storage. Serve
`glTexSubImage2D`, convertendo solo la regione modificata.

**P0-5.** Meglio Release di default e Debug come opzione per singolo gioco.

**P0-6.** Ogni riga fa `sceIoOpen`/`Write`/`Close` su ux0, compresi i primi 256 `trace()`
degli SWF. Meglio default off, con un ring buffer in RAM svuotato da un thread a bassa
priorità.

**P0-7.**
- Misurare `opt-level = 3` e `lto = "fat"` contro gli attuali `2` e `"thin"`.
- Controllare con `rustc --print cfg` che il target abiliti davvero NEON e Cortex-A9.
- Mettere dietro una feature `vita-profile` i contatori `vita_*` del fork (il `match` per
  azione in `activation.rs`, gli `Instant::now()` per fase in `player.rs`).

**P0-8.** Audio e worker condividono priorità (`base+1`) e CPU 1|2: durante un
`parallel_for` l'audio può andare in underrun. Va data all'audio una priorità più alta.

## Ottimizzazioni medie (1–3 settimane)

**M1: GC su misura per la Vita (Alto).**
- Tarare il `Pacing` di gc-arena con uno sleep factor più alto: più RAM, meno tracing.
- Fare `collect_debt` una volta per frame host, invece che a ogni `update()`
  (`core/src/player.rs:2707`; oggi gira anche su ogni evento di input).
- Usare il tempo che avanza prima del vblank come budget per il GC incrementale.

**M2: AVM1 con IR precompilata (Alto).** Il fork ha già un fast path per opcode e il caching
dei Push tramite HashMap `(movie, offset)`. Il passo successivo è decodificare ogni blocco
`DoAction` una sola volta in un `Arc<[Op]>` compatto, con constant pool già internata in
`AvmString` e salti risolti, in cache per `SwfSlice`. Spariscono parsing e hashing a ogni
esecuzione.

**M3: batching del renderer (Medio).**
- Indici u16 al posto di u32 (`src/player/vitagl_bridge.cpp:156`, `:291`).
- Raggruppare le draw testurizzate per texture.
- Deduplicare le texture dei gradienti, oggi una per ogni gradiente di ogni shape
  (`rust/ruffle_bridge/src/vita_renderer.rs:1470`).
- Riusare i vertici già trasformati quando shape, matrice e color transform non cambiano.

**M4: qualità di tessellazione per gioco (Medio).** Oggi è fissa a `StageQuality::High`
(`vita_renderer.rs:686`); Medium o Low a 960×544 riducono i vertici da trasformare.

**M5: HTTP fuori dal main thread (Medio).** `http_fetch_to_file` è sincrono dentro il poll
delle future (`rust/ruffle_bridge/src/vita_navigator.rs:554`), con timeout fino a 60 s.
Va spostato su un thread I/O dedicato.

## Refactoring Vita-native (settimane)

**R1: renderer GXM nativo.** Come passo intermedio va bene vitaGL con shader custom e VBO.
- Mesh caricate una sola volta in memoria GPU al momento di `register_shape`.
- Per ogni draw solo uniform: matrice 2×3 e color transform (mult + add). Il lavoro CPU sui
  vertici va a zero, e si risolve anche il limite del ColorTransform additivo sulle texture.
- Varianti di shader separate (color, gradient, bitmap) invece di un uber-shader; blend mode
  come varianti del patcher.
- Una sola scena per frame, parameter buffer dimensionato, ring buffer per le uniform
  sincronizzati con notifiche.

**R2: `cacheAsBitmap` e `render_offscreen` tramite render target.** Oggi `render_offscreen`
restituisce `None` e `_cache_entries` viene ignorato (`vita_renderer.rs:1497`, `:1511`).
Molti giochi Flash contano su `cacheAsBitmap`; senza, ridisegnano vettori complessi a ogni
frame. Implementarlo sistema anche `BitmapData.draw` e apre la strada ai filtri.

**R3: pipeline su tre core.** Il frame N viene renderizzato mentre gira la logica del frame
N+1. Richiede di mettere in coda anche le operazioni sulle risorse (`register_bitmap`,
`update_texture`), che oggi avvengono durante il tick.

| | Core 0 | Core 1 | Core 2 | Core 3 |
| --- | --- | --- | --- | --- |
| Oggi | Tutto il frame in serie: input, tick Ruffle, prepass, submit GL, swap | Worker: vertici, predecode; audio su CPU 1\|2 | Worker come il core 1 | Sistema |
| Proposta | Logica: tick Ruffle frame N+1, AVM, GC a budget | Render: CommandList frame N, prepass, submit GXM, upload | Servizi: audio ad alta priorità, decode, I/O, log | Sistema |

**R4: allocatore dedicato.**
- Un memblock cached tutto per Rust, con un allocatore a classi di dimensione o TLSF (per
  esempio i crate `rlsf` o `talc`) e realloc sul posto.
- Togliere il wrapping di `malloc` verso i pool vitaGL, che restano solo per i dati GPU.
- Ridimensionare la heap newlib a ciò che serve davvero a libc.

**R5: BitmapData zero-copy.** Il buffer dei pixel vive in memoria mappata dalla GPU ed è
campionato come texture lineare, con double buffering: niente upload per i giochi che
manipolano pixel a ogni frame.

## Repo e build

- Sono tracciati 29 MB di `.codex_ab/` (eboot e vpk), 15 MB di `rust/ruffle_bridge/target/`
  e `.flashvita-avm1-audit.diff` in root: vanno rimossi e aggiunti al `.gitignore`.
- Il fork Ruffle ha un solo commit (`0e30f1b`, 26 file, circa 1.900 righe). Separarlo per
  argomento (profiling, AVM1, memoria, bitmap) rende gestibili i rebase su upstream.
- Serve un benchmark ripetibile (Pacman, Cubefield e uno SWF AS3) con A/B su hardware, e la
  revisione e i flag di vitaGL registrati accanto all'archivio.

## Roadmap

1. **Misura (ore).** Verificare P0-1 e fissare la baseline sui tre SWF.
2. **Correzioni rapide (giorni).** P0-2 … P0-8, un item per commit, ciascuno con A/B.
3. **GC e AVM1 (settimane).** M1 e M2: è dove sta il grosso del divario su Cubefield,
   circa 24× rispetto al budget di 41,7 ms.
4. **Renderer GXM (settimane).** R1 insieme a M3 e M4.
5. **Architettura (settimane).** R2, R3, R4 e R5.

## Già fatto bene

- Build separate per Ruffle e shell.
- Upload parziale delle bitmap.
- Cache dello stato GL tra le draw.
- Pointer inattivo fuori dallo stage.
- Clock di gioco richiesti e loggati.
- SWF grandi in memblock dedicato.
- Fast path AVM1 per opcode.
- Profiling a finestre di 30 frame.
