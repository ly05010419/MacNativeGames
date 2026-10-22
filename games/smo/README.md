# Super Mario Odyssey (Port-Projekt)

Nativer ARM64 / Metal Port von **Super Mario Odyssey 1.3.0** (`0100000000010000`), analog zu Tears of the Kingdom und Bayonetta 3.

## Was bereits vorbereitet und verifiziert wurde:

1. **ExeFS Extraktion & Integritätsprüfung:**
   - Alle Module (`main`, `sdk`, `subsdk0`, `rtld`, `main.npdm`) aus dem Update 1.3.0 NSP extrahiert und gegen die internen PFS0/ExeFS-Hashes geprüft.
2. **Statischer ARM64 Binary-Rewrite (`d2r`):**
   - `sdk`, `subsdk0` und `main` erfolgreich für Apple Silicon modifiziert (Register x18, System Calls `svc`, Thread Pointer).
   - 0 Fehler, saubere Relocation-Tabellen mit 226.799 Relocations.
3. **RomFS Dekodierung & BKTR-Merge:**
   - Base NSP (`5.5 GB`) und Update NSP (`386 MB`) gemerged und alle 6 IVFC-Level verifiziert.
   - 4.972 Spieldateien im RomFS-Index eingebunden.
4. **Firmware Shared Fonts:**
   - Alle 6 BFTTF-Systemfonts aus Firmware 22.5.0 extrahiert und in den Boot-Prozess eingebunden.
5. **Loader & Runtime:**
   - `nn::init::Start` auf die 4-Parameter-Signatur von Nintendos frühem SDK angepasst.
   - `nnosInitialize`, SDK-Initialisierung, alle C++ Constructors (`DT_INIT`) und Eintritt in `nnMain` laufen erfolgreich durch!
   - `smo-native` Binary und die App `Super Mario Odyssey.app` wurden erfolgreich gebaut.

## Dateien & Ordnerstruktur:

- `games/smo/Super Mario Odyssey.app`: Der Launcher mit Einstellungsoberfläche.
- `games/smo/SMO_build/`: Der Arbeitsordner mit den extrahierten Assets, RomFS und Binaries.
- `games/smo/SMO_build/build/smo-native`: Das kompilierte native Spielprogramm.

## Starten / Testen:

```sh
# GUI Launcher öffnen:
open "games/smo/Super Mario Odyssey.app"

# Oder direkt aus der Konsole starten:
cd games/smo && make boot
```
