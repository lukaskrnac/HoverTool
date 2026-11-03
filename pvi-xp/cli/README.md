# pvi_cli – konzolový PVI klient (bez .NET, beží na Windows XP)

Malý **natívny** (C, 32-bit, len `KERNEL32.dll` + `ADVAPI32.dll`) nástroj, ktorý číta a zapisuje
procesné premenné PLC B&R cez **PVI**. `PviCom.dll` **nená** v sebe — nájde si ju za behu
a volá ju dynamicky (`LoadLibraryA` + `GetProcAddress`). Preto:

* na **preklad** nepotrebuješ hlavičky ani `.lib` z PVI,
* vo výslednom `.exe` nie je žiadna závislosť na .NET ani na CRT,
* knižnicu **nemusíš nikam kopírovať** — program si ju sám nájde.

```
pvi_cli.exe info
pvi_cli.exe read  gTest.iCounter --ip 192.168.1.10
pvi_cli.exe write gTest.iCounter 42 --ip 192.168.1.10
```

## Hotové `.exe`

V tomto priečinku je `pvi_cli.exe` (41 KB), zostavený a overený takto:

| Vlastnosť | Hodnota |
|---|---|
| formát | PE32, **i386** (32-bit) |
| subsystém | console, **verzia 5.01** (Windows XP) |
| importy | `KERNEL32.dll`, `ADVAPI32.dll` iba |
| CRT / .NET | žiadne |

> Spustenie sa **nedá overiť** v prostredí, kde som to staval (Linux, bez Windows/PLC).
> Prvý beh na XP je teda prvý reálny test — pozri *Riešenie problémov* nižšie.

## Použitie

```
pvi_cli info                              # verzia PVI + stav licencie
pvi_cli read  <premenna>                  # vypise hodnotu na stdout
pvi_cli write <premenna> <hodnota>        # zapise hodnotu
```

Voliteľné parametre:

| Parameter | Význam |
|---|---|
| `--ip <adresa>` | IP PLC (default `192.168.1.10`) |
| `--proto ansl\|ina` | `ansl` = AR ≥ 4.08 (port 11169), `ina` = starší AR (INA2000) |
| `--station <n>` | INA: cieľová stanica PLC `/DA=` (default 2) |
| `--src <n>` | INA: stanica PC `/SA=` (default 113) |
| `--com <n>` + `--baud <n>` | INA po sériovej linke (napr. `--com 1 --baud 57600`) |
| `--cpu <nazov>` | názov CPU objektu v PVI (default `cpu`) |
| `--task <nazov>` | ak je premenná **lokálna** v úlohe (task) |
| `--dll <cesta>` | vynúti konkrétny `PviCom.dll` |
| `--timeout <s>` | ako dlho čakať na spojenie (default 5 s) |
| `--raw` | vypíše dáta ako HEX (štruktúry, neznáme typy) |
| `--verbose` | diagnostika na `stderr` (čo sa vytvára, aké eventy chodia) |

Príklady:

```bat
pvi_cli.exe read gTest.iCounter --ip 192.168.1.10
pvi_cli.exe write gTest.iCounter 1500 --ip 192.168.1.10
pvi_cli.exe read gTest.rTeplota --ip 192.168.1.20 --proto ina --station 2
pvi_cli.exe read gTest.sText --ip 192.168.1.10 --verbose
pvi_cli.exe info --verbose
```

Použitie v dávke (stdout = len hodnota, chyby idú na stderr):

```bat
for /f %%v in ('pvi_cli.exe read gTest.iCounter --ip 192.168.1.10') do set HODNOTA=%%v
```

Návratové kódy: `0` ok · `1` parametre · `2` nenašiel DLL · `3` init PVI ·
`4` vytvorenie objektov · `5` čítanie · `6` zápis.

## Spúšťanie (pozor: je to konzolový program!)

Dvojklik v Prieskumníku otvorí okno, program prebehne a **okno sa zavrie** — to je normálne
správanie konzolovej aplikácie, nie chyba. Preto:

* **od verzie „s pauzou“** program zistí, že bol spustený samostatne (je jediný proces
  v konzole) a na konci vypíše `navratovy kod: N` a **počká na Enter**;
* **odporúčané je spúšťať z `cmd`** — vidíš celý výstup a nič nezmizne:

```bat
Start -> Spustit... -> cmd
cd /d C:\pvi
pvi_cli.exe info --verbose
```

* v dávke/skripte pridaj `--nopause`, aby sa nič nečakalo:

```bat
pvi_cli.exe read gTest.iCounter --ip 192.168.1.10 --nopause
```

Prvý diagnostický beh, ktorý odporúčam (ne potrebuje PLC ani vedieť premennú):

```bat
pvi_cli.exe info --verbose
```

Vypíše, ktorú `PviCom.dll` našiel, či sa pripojil na PVI Manager, verziu PVI a stav licencie.

## Ako si nájde knižnicu

Poradie (prvé úspešné vyhráva):

1. `--dll <cesta>`,
2. `%PVI_INSTALL_FOLDER%` (premenná, ktorú PVI nastavuje) – skúša `<dir>`, `<dir>\bin`, `<dir>\Bin`, `<dir>\PVI`,
3. **registry**: `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\PviMan.exe`
   (PVI 4) alebo `Pvi6Man.exe` (PVI 6) → hodnota `Path`,
4. časté cesty: `C:\BrAutomation\PVI[\V4.12 …]`, `C:\Program Files\BrAutomation\PVI`, …
5. už len podľa mena (PATH / aktuálny priečinok).

Hľadajú sa **`PviCom.dll`** a **`Pvi6Com.dll`** (32-bitové). 64-bitové `PviCom64.dll`
nepoužívame — program je 32-bitový.

## Ako to funguje (PVI API)

Program vytvorí rovnakú hierarchiu objektov ako PVI Monitor / PviServices:

```
@Pvi
 └── LNANSL  (alebo LNINA2)                       POBJ_LINE   CD=LNANSL
      └── TCP                                     POBJ_DEVICE CD=/IF=TcpIp     (INA: /IF=tcpip /SA=113)
           └── cpu                                POBJ_CPU    CD=/IP=<ip>       (INA: /DAIP=<ip> /DA=<stanica>)
                └── gTest.iCounter                POBJ_PVAR   CD=<premenna> RF=0
```

Volané funkcie z `PviCom.dll` (všetko cez `GetProcAddress`, takže kompatibilné s PVI 4 aj 6):

| Funkcia | Účel |
|---|---|
| `PviXInitialize` / `PviXDeinitialize` | pripojenie klienta na PVI Manager |
| `PviXSetGlobEventMsg` | globálne eventy (connect/disconnect/arrange) |
| `PviXCreate` / `PviXUnlink` | vytvorenie/zrušenie procesného objektu |
| **`PviXRead`** / **`PviXWrite`** | **synchrónne čítanie/zápis** (`POBJ_ACC_DATA`) |
| `PviXRead` s `POBJ_ACC_TYPE_EXTERN/INTERN` | zistenie typu premennej (VT/VL/VN) |
| `PviXGetNextResponse` / `PviXGetResponseInfo` / `PviXReadResponse` | fronta odpovedí a eventov |

Pretože `PviXRead`/`PviXWrite` sú synchrónne, **nepotrebuje** okno ani message loop —
stačí konzola. (Na rozdiel od .NET `PviServices`, ktoré message pump vyžaduje.)

### Podporované typy

| Typ | Čítanie | Zápis |
|---|---|---|
| `boolean` | `TRUE`/`FALSE` | `0/1`, `TRUE/FALSE` |
| `i8 i16 i32 i64`, `u8 u16 u32 u64` | číslo | `123`, `-5`, `0x1F`, `16#FF`, `2#1010`, `8#77` |
| `f32 f64` | číslo (≈7 platných cifier) | `1.5`, `-2e3` |
| `string` / `wstring` | v úvodzovkách | text (orezaný na dĺžku premennej) |
| pole (`VN>1`) | `[0]=1 [1]=2 …` | jedna hodnota sa zapíše do všetkých prvkov |
| `dt date tod time` | číslo + typ | číslo |
| `struct` | HEX (`--raw` ekvivalent) | **nepodporované** — zapisuj jednotlivé membery |

## Obmedzenia

* **Licencia PVI**: na „neB&R“ PC beží PVI v **trial režime 2 hodiny** (stav ukáže `pvi_cli info`).
* Jeden beh = **jedna premenná** (je to CLI nástroj; pre veľa premenných použi .NET verziu).
* Konvencia volania je `__stdcall`. Ak by `PviCom.dll` bola `cdecl` (nepravdepodobné),
  prelož s `-DPVI_CALL_CDECL`.
* Bez TLS — len v dôveryhodnej sieti.

## Preklad (ak chceš build zo zdrojáku)

**MinGW-w64 (odporúčané, nastaví správny subsystém sám):**

```bat
i686-w64-mingw32-gcc -O2 -ffreestanding -fno-builtin -nostdlib ^
  -Wl,-e,mainCRTStartup -Wl,--subsystem,console:5.01 ^
  -lkernel32 -ladvapi32 -o pvi_cli.exe pvi_cli.c
```

**Zig** (takto je zostavený priložený `.exe`):

```bat
zig cc -target x86-windows-gnu -O2 -ffreestanding -fno-builtin -nostdlib ^
  -Wl,--entry=mainCRTStartup -lkernel32 -ladvapi32 -o pvi_cli.exe pvi_cli.c
rem zig nevie nastavit verziu subsystemu -> doplni sa v PE hlavicke na 5.01 (XP):
rem   offset PE+24+48 = WORD major/minor subsystem version
```

Bez `-nostdlib` by sa program zlinkoval proti UCRT (`api-ms-win-crt-*.dll`), ktorý na XP
neexistuje — preto sú v zdrojáku vlastné `memcpy/memset/strlen` a vlastné formátovanie čísel.

## Riešenie problémov

| Kód / príznak | Príčina |
|---|---|
| `2` – nenájde `PviCom.dll` | PVI nie je nainštalované, alebo použi `--dll <cesta>` |
| `3` – `PviXInitialize` ≠ 0 | nebeží **PVI Manager** (`PviMan.exe` / služba `PviManager`) |
| `4` – `PviXCreate` ≠ 0 | zlá IP / protokol (skús `--proto ina`), alebo PLC neodpovedá |
| `5` – čítanie zlyhalo | zlý názov premennej, alebo premenná nie je globálna (použi `--task`) |
| 0x…„not a valid Win32 application“ | 32-bit verzia DLL vs 64-bit proces (a naopak) |
| po ~2 h chyby | skončil trial režim PVI |

Overenie „naslepo“, bez PLC: `pvi_cli.exe info --verbose` — ukáže, ktorú DLL našiel
a či sa pripojil na PVI Manager.
