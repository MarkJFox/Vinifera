# Custom Paletten

Stand: 2026-11-26, Branch `feature/custom-palettes`. Gebäudekörper und Gebäudeanimationen im Spiel getestet. Freie Animationen und Remap im Spiel getestet. Terrain im Spiel getestet. Overlays, Mauern, Projektile und Cameos im Spiel getestet. Voxel, Voxel-Projektile und Partikel kompiliert, aber noch **nicht im Spiel getestet**.

Hintergrund und Analyse: `../../analysis/CUSTOM-PALETTEN-BERICHT.md`.

## Verwendung

In der Bild-Sektion der ART.INI (der Sektion, auf die `Image=` zeigt) bei Einheiten, Gebäuden und Terrain, oder in der ART.INI-Sektion einer Animation:

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
- **Terrain** (Bäume, Felsen und andere TerrainTypes) mit `Palette=` wird mit dieser Palette statt der Iso-Palette des Theaters gezeichnet. Das Licht kommt wie bisher von der Zelle: Für jede Licht-Tönung gibt es einen eigenen Converter, wie bei den Iso-Tiles, und der Ionensturm wird vom Zell-Drawer übernommen. Keine Hausfarben. Terrain mit `SpawnsTiberium=yes` behält den Tiberium-Drawer, der Schatten wird wie bisher gezeichnet.
- **Overlays** (Felsen, Kisten, Gestrüpp usw.) mit `Palette=` in ihrer Bild-Sektion werden mit dieser Palette gezeichnet, beleuchtet wie die Zelle (wie Terrain). Tiberium, Adern (Veins) und Brücken behalten die Paletten des Spiels.
- **Mauern** (`Wall=yes`) mit `Palette=` bekommen die Hausfarben-Rampe (Einträge 16–31) wie Einheiten. Das Farbschema ist dasselbe, das das Spiel für Mauern nimmt: die Farbe des lokalen Spielers.
- **Projektile** (SHP) mit `Palette=` in ihrer Bild-Sektion ersetzen `PALETTE.PAL` bzw. `ANIM.PAL` (`AnimPalette=yes`). Neu ist `FirersPalette=yes` in der Rules-Sektion des Projektils, wie in Yuri's Revenge:
  - Nur `Palette=`: eigene Palette ohne Remap.
  - `Palette=` und `FirersPalette=yes`: eigene Palette in der Hausfarbe des Schützen.
  - Nur `FirersPalette=yes`: UNIT-Palette in der Hausfarbe des Schützen.
  - Ist der Schütze nicht mehr da, gilt das erste Farbschema. Der Schatten bleibt unverändert. Voxel-Projektile siehe unten.
- **Cameos**: `CameoPalette=` in der Bild-Sektion eines TechnoTypes (wie in Ares) ersetzt `CAMEO.PAL` für das SHP-Cameo, in der Sidebar und in der Produktionsanzeige der Fabrik. Für Superwaffen gilt `SidebarPalette=` in ihrer Rules-Sektion. PNG/PCX-Cameos sind nicht betroffen. Cameo-Paletten am besten mit Endung angeben (`CameoPalette=mycameo.pal`), sonst wird wie überall das Theater-Suffix versucht.
- **Voxel** (Einheiten und Flugzeuge mit `Voxel=yes`): `Palette=` in der Bild-Sektion gilt auch für sie. Das Spiel rendert Voxel in eine 8-Bit-Fläche mit Palettenindizes aus der VPL und blittet sie dann mit dem Converter des Farbschemas. Dieser Converter wird durch die eigene Palette mit derselben Hausfarben-Rampe ersetzt, für frisch gerenderte, gecachte und zusammengesetzte Voxel (Turm, Lauf). Die VPL schattiert über die Indizes der UNIT-Palette. Eine Voxel-Palette muss deshalb dieselbe Anordnung der Farbverläufe haben, sonst stimmt die Schattierung nicht. Gebäude sind keine Voxel-Typen, ihr `Palette=` gilt also nicht für Voxel-Türme. Schatten bleiben unverändert.
- **Voxel-Projektile**: `Palette=` in der Bild-Sektion ersetzt die Palette, die Hausfarben-Rampe kommt aus dem Farbschema des Projektils (`Color=`), mit `FirersPalette=yes` aus dem Haus des Schützen. Nur `FirersPalette=yes` färbt die Palette des Spiels in der Farbe des Schützen.
- **Partikel** mit `Palette=` in ihrer Bild-Sektion werden mit dieser Palette statt `ANIM.PAL` gezeichnet. Keine Hausfarben.
- **Hausfarben (nur Techno-Objekte):** Die Einträge 16–31 werden mit der Hausfarbe des Besitzers ersetzt, genau wie bei der UNIT-Palette. Die Farben an diesen Stellen der eigenen Palette werden also nie angezeigt.
- **Licht (Techno-Objekte):** Helligkeit und Ionensturm-Tönung wirken wie bei normalen Objekten.
- **Leuchtfarben 240–254:** Diese Einträge werden wie im Original (TS und YR, Tönungsmaske bei `0x70CEAC`) nicht abgedunkelt. Wer dort normale Farben ablegt oder sie im SHP benutzt, sieht helle Pixel bei dunklem Licht. Für Flächen, die nicht leuchten sollen, im SHP andere Indizes verwenden.
- **Sonderfälle bleiben:** Gebäude mit `TerrainPalette=yes` nutzen weiter die Iso-Palette der Zelle. Ein gesetztes `Palette=` wird für sie und ihre Animationen ignoriert.
- **Verkleidete Infanterie:** Sie wird für Gegner mit der Hausfarbe des Betrachters und mit ihrer eigenen Palette gezeichnet.

## Technik

- `PaletteName` in den Erweiterungen von TechnoType, AnimType, TerrainType, OverlayType, BulletType und ParticleType hält den Namen aus `Palette=`. Dazu kommen `TechnoTypeClassExtension::CameoPaletteName`, `SuperWeaponTypeClassExtension::SidebarPaletteName` und `BulletTypeClassExtension::IsFirersPalette`. Sie werden mit dem Spielstand gespeichert.
- `CustomPalette` (`src/new/custompalette/`) lädt jede Palette beim ersten Gebrauch. Pro Farbschema baut es einen `LightConvertClass` mit der Rampe der Hausfarbe des Schemas, derselben Zahl an Helligkeitsstufen und derselben Tönungsmaske wie das Spiel. Für Animationen gibt es pro Palette einen einfachen `ConvertClass` mit 63 Helligkeitsstufen, gebaut wie der `AnimDrawer` des Spiels. Für Terrain gibt es pro Palette und Licht-Tönung einen `LightConvertClass`, gebaut wie die Iso-Tile-Drawer (`IsometricTileTypeClass::Find_Or_Make_Drawer`). Tönung und Ionensturm-Zustand liest es aus dem Zell-Drawer (Offsets `0x1A0`–`0x1B8`, aus `LightConvertClass::Apply_Tint`). Converter werden nicht gespeichert und bei `Init_Theater` verworfen.
- Ionensturm: Beim ersten Gebrauch nach Beginn oder Ende eines Sturms wird die Tönung nachgezogen (`Apply_Tint`).
- Hooks: `0x00634F40` (normaler Converter) und `0x00635075` (verkleidete Infanterie) in `Techno_Draw_Object`. Für Gebäudeanimationen `0x004363CC` in `BuildingClass::Create_Anim`, `0x00435ADE` in `BuildingClass::Animation_AI` (setzt den Drawer in jedem Logik-Frame neu) und `0x00436668` in `BuildingClass::Update_Anim_Appearance` (dort ist `Set_Anim_Drawer` eingebettet). Für freie Animationen `0x004148AA` (Wahl von `AnimDrawer`/`AltPalette` in `AnimClass::Draw_It`) und `0x00414AAB` (gekachelte Animationen). Für Terrain `0x0063FB54` in `TerrainClass::Draw_It` (dort lädt das Spiel den Zell-Drawer). Für Overlays und Mauern `0x00455B1B` in `CellClass::Draw_Overlay` (gemeinsamer Draw_Shape-Aufruf, EDX hält den Converter). Für Projektile `0x00445EAA` in `BulletClass::Draw_It` (EBX hält `NormalDrawer` oder `AnimDrawer`). Für Voxel `0x00635D4E` (Render-Pfad in `TechnoClass::Draw_Voxel`), `0x00636037` (`Techno_Blit_Voxel`, Cache) und `0x00652108` (`UnitClass::Unit_Blit_Voxel`, zusammengesetzte Voxel). Für Voxel-Projektile `0x004473C6` (merkt sich den Bullet), `0x004473EE` (Blitter) und `0x00447416` (`Blit_Block`) in `BulletClass::Draw_Voxel`. Für Partikel `0x005A46DA` in `ParticleClass::Draw_It`. Cameos werden in `SidebarStripView::Draw_Cameo` und in der Fabrik-Anzeige in `buildingext_hooks.cpp` über `CustomPalette::Cameo_Converter` gezeichnet.

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
13. Baum mit `Palette=` neben einem Baum ohne, bei Tag und Nacht, unter einer Lampe (Lichtquelle) und im Ionensturm.
14. Terrain mit `Palette=` nach Speichern und Laden.
15. Felsen oder anderes Overlay mit `Palette=`, bei Tag, Nacht und unter einer Lampe.
16. Mauer mit `Palette=` bei zwei Hausfarben (lokaler Spieler wechseln).
17. Projektil mit `Palette=`, mit `Palette=` und `FirersPalette=yes`, und nur mit `FirersPalette=yes`, von zwei Häusern abgefeuert.
18. Einheit mit `CameoPalette=` in der Sidebar und in der Fabrik-Anzeige, Superwaffe mit `SidebarPalette=`.
19. Voxel-Einheit mit `Palette=` bei zwei Hausfarben, auch getarnt bzw. halbtransparent, mit Turm und nach dem Cache (stehen lassen, dann bewegen).
20. Voxel-Projektil mit `Palette=`, mit und ohne `FirersPalette=yes`.
21. Partikel (Rauch, Feuer) mit `Palette=`.
