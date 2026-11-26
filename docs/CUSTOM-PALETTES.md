# Custom Paletten – Stufe 1: SHP-Objekte

Stand: 2026-11-25, Branch `feature/custom-palettes`. Gebäudekörper im Spiel getestet. Gebäudeanimationen kompiliert (RelWithDebInfo), aber noch **nicht im Spiel getestet**.

Hintergrund und Analyse: `../../analysis/CUSTOM-PALETTEN-BERICHT.md`.

## Verwendung

In der Bild-Sektion der ART.INI (der Sektion, auf die `Image=` zeigt):

```ini
[MYUNIT]
Palette=custom
```

- **Ohne Endung** (wie in Yuri's Revenge): Theater-Suffix und `.PAL` werden angehängt, im Temperate also `CUSTOMTEM.PAL`, im Snow `CUSTOMSNO.PAL`. Fehlt die Theater-Variante, wird `CUSTOM.PAL` geladen.
- **Mit Endung** (`Palette=custom.pal`): Die Datei wird genau so geladen, unabhängig vom Theater.
- Fehlt die Datei, wird das Objekt wie bisher mit der UNIT-Palette des Theaters gezeichnet. Im Debug-Log steht eine Warnung.

Die Palette ist eine normale TS-`.PAL`-Datei (256 Farben, 6 Bit pro Kanal).

## Was gilt

- **Erfasst:** alles, was über `TechnoClass::Techno_Draw_Object` gezeichnet wird. Das sind SHP-Infanterie, SHP-Fahrzeuge, SHP-Flugzeuge und Gebäudekörper (auch Bauphase, Fundament und die Animation unter dem Tor).
- **Gebäudeanimationen** (`ActiveAnim`, `IdleAnim` usw.) mit `ShouldUseCellDrawer=yes` (Standard) übernehmen die Palette ihres Gebäudes. Animationen mit `ShouldUseCellDrawer=no` nutzen wie bisher `ANIM.PAL`.
- **Nicht erfasst:** Voxel, freie Animationen, Projektile und Cameos.
- **Hausfarben:** Die Einträge 16–31 werden mit der Hausfarbe des Besitzers ersetzt, genau wie bei der UNIT-Palette. Die Farben an diesen Stellen der eigenen Palette werden also nie angezeigt.
- **Licht:** Helligkeit und Ionensturm-Tönung wirken wie bei normalen Objekten.
- **Leuchtfarben 240–254:** Diese Einträge werden wie im Original (TS und YR, Tönungsmaske bei `0x70CEAC`) nicht abgedunkelt. Wer dort normale Farben ablegt oder sie im SHP benutzt, sieht helle Pixel bei dunklem Licht. Für Flächen, die nicht leuchten sollen, im SHP andere Indizes verwenden.
- **Sonderfälle bleiben:** Gebäude mit `TerrainPalette=yes` nutzen weiter die Iso-Palette der Zelle. Ein gesetztes `Palette=` wird für sie und ihre Animationen ignoriert.
- **Verkleidete Infanterie:** Sie wird für Gegner mit der Hausfarbe des Betrachters und mit ihrer eigenen Palette gezeichnet.

## Technik

- `TechnoTypeClassExtension::PaletteName` hält den Namen aus `Palette=`. Er wird mit dem Spielstand gespeichert.
- `CustomPalette` (`src/new/custompalette/`) lädt jede Palette beim ersten Gebrauch. Pro Farbschema baut es einen `LightConvertClass` mit der Rampe der Hausfarbe des Schemas, derselben Zahl an Helligkeitsstufen und derselben Tönungsmaske wie das Spiel. Converter werden nicht gespeichert und bei `Init_Theater` verworfen.
- Ionensturm: Beim ersten Gebrauch nach Beginn oder Ende eines Sturms wird die Tönung nachgezogen (`Apply_Tint`).
- Hooks: `0x00634F40` (normaler Converter) und `0x00635075` (verkleidete Infanterie) in `Techno_Draw_Object`. Für Gebäudeanimationen `0x004363CC` in `BuildingClass::Create_Anim`, `0x00435ADE` in `BuildingClass::Animation_AI` (setzt den Drawer in jedem Logik-Frame neu) und `0x00436668` in `BuildingClass::Update_Anim_Appearance` (dort ist `Set_Anim_Drawer` eingebettet).

## Testplan

1. Zwei verschiedene eigene Paletten und ein Vanilla-Objekt auf derselben Karte.
2. Dieselben Typen bei zwei Hausfarben.
3. Ionensturm beginnen und enden lassen.
4. Theater ohne passende Paletten-Variante (Fallback auf `NAME.PAL` bzw. die Standardpalette).
5. Gebäude mit `TerrainPalette=yes` und `Palette=`.
6. Verkleidete Infanterie aus Sicht des Besitzers und eines Gegners.
7. Speichern und Laden, danach Theaterwechsel durch eine neue Karte.
8. Gebäude mit Animationen (aktiv, im Leerlauf, beschädigt) und eigener Palette, auch nach Ionensturm und Stromausfall.
