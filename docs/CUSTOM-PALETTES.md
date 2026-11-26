# Custom Paletten – SHP-Objekte und Animationen

Stand: 2026-11-26, Branch `feature/custom-palettes`. Gebäudekörper und Gebäudeanimationen im Spiel getestet. Freie Animationen (`[AnimType] Palette=`) kompiliert, aber noch **nicht im Spiel getestet**.

Hintergrund und Analyse: `../../analysis/CUSTOM-PALETTEN-BERICHT.md`.

## Verwendung

In der Bild-Sektion der ART.INI (der Sektion, auf die `Image=` zeigt), oder in der ART.INI-Sektion einer Animation:

```ini
[MYUNIT]
Palette=custom

[MYEXPLOSION]
Palette=fire
```

- **Ohne Endung** (wie in Yuri's Revenge): Theater-Suffix und `.PAL` werden angehängt, im Temperate also `CUSTOMTEM.PAL`, im Snow `CUSTOMSNO.PAL`. Fehlt die Theater-Variante, wird `CUSTOM.PAL` geladen.
- **Mit Endung** (`Palette=custom.pal`): Die Datei wird genau so geladen, unabhängig vom Theater.
- Fehlt die Datei, wird das Objekt wie bisher gezeichnet (UNIT-Palette des Theaters, bei Animationen `ANIM.PAL`). Im Debug-Log steht eine Warnung.

Die Palette ist eine normale TS-`.PAL`-Datei (256 Farben, 6 Bit pro Kanal).

## Was gilt

- **Erfasst:** alles, was über `TechnoClass::Techno_Draw_Object` gezeichnet wird. Das sind SHP-Infanterie, SHP-Fahrzeuge, SHP-Flugzeuge und Gebäudekörper (auch Bauphase, Fundament und die Animation unter dem Tor).
- **Gebäudeanimationen** (`ActiveAnim`, `IdleAnim` usw.) mit `ShouldUseCellDrawer=yes` (Standard) übernehmen die Palette ihres Gebäudes. Ein `Palette=` an der Animation selbst gilt für sie nicht. Animationen mit `ShouldUseCellDrawer=no` nutzen ihre eigene Palette, sonst `ANIM.PAL`.
- **Freie Animationen** (Explosionen, Mündungsfeuer, Rauch usw.) mit `Palette=` werden mit dieser Palette statt mit `ANIM.PAL` gezeichnet. Das gilt auch für gekachelte Animationen (`Tiled=yes`). Ohne `AltPalette` gibt es wie bei `ANIM.PAL` keine Hausfarben und keine Ionensturm-Tönung, die Helligkeit kommt wie bisher aus der Zelle (`UseNormalLight`).
- **Hausfarben bei Animationen (`AltPalette=yes`):** Pro Animation in der ART.INI wählbar.
  - `Palette=` und `AltPalette=yes`: eigene Palette, Einträge 16–31 in der Hausfarbe des Besitzers, mit Ionensturm-Tönung wie bei Einheiten.
  - Nur `AltPalette=yes`: UNIT-Palette in der Hausfarbe des Besitzers. Im Original nahm das Spiel hier immer das erste Farbschema, auch Yuri's Revenge (erst Phobos hat das korrigiert).
  - Besitzer ist das Objekt, an dem die Animation hängt (zum Beispiel die Einheit am Fallschirm). Animationen ohne Besitzer, etwa die meisten Explosionen, nehmen wie bisher das erste Farbschema.
  - Gekachelte Animationen ignorieren `AltPalette` wie im Original, außer zusammen mit `Palette=`.
  - Beispiel: Explosionen mit `Palette=fire` ohne Remap, ein Fallschirm mit `Palette=para` und `AltPalette=yes` in Hausfarbe.
- **Ausnahmen bei Animationen:** Tiberium-Adern (`IsVeins`), an Zellen gebundene Animationen und Animationen, die vom Gebäude einen Drawer bekommen haben, behalten den Converter des Spiels.
- **Nicht erfasst:** Voxel, Projektile, Partikel, Terrain, Overlays und Cameos.
- **Hausfarben (nur Techno-Objekte):** Die Einträge 16–31 werden mit der Hausfarbe des Besitzers ersetzt, genau wie bei der UNIT-Palette. Die Farben an diesen Stellen der eigenen Palette werden also nie angezeigt.
- **Licht (Techno-Objekte):** Helligkeit und Ionensturm-Tönung wirken wie bei normalen Objekten.
- **Leuchtfarben 240–254:** Diese Einträge werden wie im Original (TS und YR, Tönungsmaske bei `0x70CEAC`) nicht abgedunkelt. Wer dort normale Farben ablegt oder sie im SHP benutzt, sieht helle Pixel bei dunklem Licht. Für Flächen, die nicht leuchten sollen, im SHP andere Indizes verwenden.
- **Sonderfälle bleiben:** Gebäude mit `TerrainPalette=yes` nutzen weiter die Iso-Palette der Zelle. Ein gesetztes `Palette=` wird für sie und ihre Animationen ignoriert.
- **Verkleidete Infanterie:** Sie wird für Gegner mit der Hausfarbe des Betrachters und mit ihrer eigenen Palette gezeichnet.

## Technik

- `TechnoTypeClassExtension::PaletteName` und `AnimTypeClassExtension::PaletteName` halten den Namen aus `Palette=`. Sie werden mit dem Spielstand gespeichert.
- `CustomPalette` (`src/new/custompalette/`) lädt jede Palette beim ersten Gebrauch. Pro Farbschema baut es einen `LightConvertClass` mit der Rampe der Hausfarbe des Schemas, derselben Zahl an Helligkeitsstufen und derselben Tönungsmaske wie das Spiel. Für Animationen gibt es pro Palette einen einfachen `ConvertClass` mit 63 Helligkeitsstufen, gebaut wie der `AnimDrawer` des Spiels. Converter werden nicht gespeichert und bei `Init_Theater` verworfen.
- Ionensturm: Beim ersten Gebrauch nach Beginn oder Ende eines Sturms wird die Tönung nachgezogen (`Apply_Tint`).
- Hooks: `0x00634F40` (normaler Converter) und `0x00635075` (verkleidete Infanterie) in `Techno_Draw_Object`. Für Gebäudeanimationen `0x004363CC` in `BuildingClass::Create_Anim`, `0x00435ADE` in `BuildingClass::Animation_AI` (setzt den Drawer in jedem Logik-Frame neu) und `0x00436668` in `BuildingClass::Update_Anim_Appearance` (dort ist `Set_Anim_Drawer` eingebettet). Für freie Animationen `0x004148AA` (Wahl von `AnimDrawer`/`AltPalette` in `AnimClass::Draw_It`) und `0x00414AAB` (gekachelte Animationen).

## Testplan

1. Zwei verschiedene eigene Paletten und ein Vanilla-Objekt auf derselben Karte.
2. Dieselben Typen bei zwei Hausfarben.
3. Ionensturm beginnen und enden lassen.
4. Theater ohne passende Paletten-Variante (Fallback auf `NAME.PAL` bzw. die Standardpalette).
5. Gebäude mit `TerrainPalette=yes` und `Palette=`.
6. Verkleidete Infanterie aus Sicht des Besitzers und eines Gegners.
7. Speichern und Laden, danach Theaterwechsel durch eine neue Karte.
8. Gebäude mit Animationen (aktiv, im Leerlauf, beschädigt) und eigener Palette, auch nach Ionensturm und Stromausfall.
9. Explosion oder Waffenanimation mit `Palette=`, bei Tag und Nacht (Zellhelligkeit).
10. Gekachelte Animation (`Tiled=yes`) und Animation mit `AltPalette=yes` jeweils mit `Palette=`.
11. Gebäudeanimation mit `ShouldUseCellDrawer=no` und eigenem `Palette=`.
12. Fallschirm (Paradrop) mit `AltPalette=yes`, einmal mit und einmal ohne `Palette=`, bei zwei Hausfarben.
