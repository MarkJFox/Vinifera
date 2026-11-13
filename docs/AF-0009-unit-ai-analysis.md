# AF-0009 – Bauziele und künftige Einheitensteuerung

Die nativen Befunde beziehen sich auf das lokale x86-`game.exe` unter
`D:/DEVELOPMENT/TS Aftermath` mit SHA-256
`32438f8af2a93be67123a69183abd50f172749ef7717e50d0b18b0fa26b917ec`.
OpenTS dient als ergänzende Rekonstruktion.

## A. Bauziel nach einem Stromauftrag

**NACHGEWIESEN.** Die B1-Stromauflösung und die B2-Auswahl lieferten dem C3-Planer
den unmittelbar baubaren Typ. Bei Strommangel war das der Stromerzeuger.
`Apply_Advanced_AI_Building_Plan` schrieb diesen Typ nach `BuildStructure`.
`DevelopmentOrder` enthielt ebenfalls nur den aktuellen Auftrag. Der
Abschluss der Fabrik löste danach eine reguläre Neuwahl aus. Bei Raffinerien
blieb das Feld als `FollowupTarget` erhalten, der bereits geprüfte Standort
wurde im Kraftwerkszweig verworfen. Der Lauf vom 12.11.2026 zeigt das konkret
für Nod und das Feld `(83,195)`. Das Kraftwerk entstand, anschließend fand
die Stichprobensuche keinen brauchbaren Sammlerweg. Ob das Kraftwerk den
konkreten Weg verbaute, ist **UNGEKLÄRT**.
[`Prioritize_Advanced_AI_Power`, Zeile 1196](../src/extensions/house/houseext_advancedai.cpp#L1196) ·
[`Probe_Expansion_Target`, Zeile 1525](../src/extensions/house/houseext_resources.cpp#L1525) ·
[Ingame-Log, Zeile 1566](<D:/DEVELOPMENT/TS Aftermath/Debug/DEBUG_12-11-2026_15-45-55.LOG:1566>)

**NACHGEWIESEN im Patch.** Der gewählte C3-Plan übergibt genau ein ursprüngliches
Bauziel als Abhängigkeit des Stromauftrags. `BuildStructure` bleibt der einzige
ausführbare Gebäudeauftrag. Der Factory-Start bindet das konkrete Stromprodukt
über seine Objekt-ID. Erst erfolgreicher Abschluss desselben Produkts gibt
die Nachprüfung frei. Der Rückruf erfasst Typ und ID vor der nativen Platzierung,
da eine installierte Turbine ihr Produktionsobjekt verbrauchen kann. Die
vorhandene Auswahl von Turbine, erweitertem und regulärem Kraftwerk bleibt
zuständig. Weitere benötigte Stromaufträge behalten das gleiche Ziel bis zur
ursprünglichen Frist von drei Simulationsminuten.
[`Resume_Advanced_AI_Power_Goal`, Zeile 772](../src/extensions/house/houseext_advancedai.cpp#L772) ·
[`BuildingClassExt::_Factory_AI`, Zeile 1386](../src/extensions/building/buildingext_hooks.cpp#L1386)

Dringende Verteidigung, Stromwiederherstellung und Wiederaufbau werden zuerst
geprüft. Danach bewertet der Planer das gemerkte Ziel vor dem regulären
optionalen Ausbau. Er prüft Besitz beziehungsweise installierte Upgrades,
Baubarkeit, Bauhof, aktuelle Wirtschaft, Geld und Strom. Bei fehlgeschlagener
Stromproduktion, abgelaufener Frist oder einem inzwischen entfallenen Ziel
läuft die normale Planung weiter. Eine verlorene Upgrade-Basis fällt durch
die bestehende Upgrade-Platzprüfung. Ein Raffinerieziel bekommt zusätzlich
eine erneute Standort- und Vorratsprüfung. Der bestätigte Standort fließt
bevorzugt in die native Suche ein. Die gerichtete Kraftwerksplatzierung
prüft Kandidaten auf Überschneidung mit dem Raffineriegrundriss und auf
einen verbleibenden Sammlerweg. Der native Platzierungsfallback steht
weiterhin zur Verfügung, falls die gerichtete Suche kein Ergebnis liefert.
[`Preserves_Refinery_Site`, Zeile 801](../src/extensions/house/houseext_resources.cpp#L801) ·
[`Resume_Advanced_AI_Refinery`, Zeile 1663](../src/extensions/house/houseext_resources.cpp#L1663)

**Save/Load.** Das Ziel liegt im bestehenden flüchtigen C3-Hauszustand.
Das Saveformat bleibt gleich. Beim Laden werden die Planungsdaten verworfen
und aus dem Spielzustand neu bewertet. Ein Save während des Stromauftrags
garantiert daher keine Wiederaufnahme exakt desselben Ziels. Planerrevision 8
fließt bei `UseAdvancedAI=yes` in den Multiplayer-Abgleich ein.
[Load-Reset, Zeile 1168](../src/vinifera/vinifera_saveload.cpp#L1168) ·
[FRAMESYNC, Zeile 41](../src/extensions/event/eventext_hooks.cpp#L41)

**Manuelle Abnahme.** Tech-Center oder Radar mit zu wenig Strom einplanen.
Den Ablauf mit Turbine, erweitertem und regulärem Kraftwerk vergleichen.
`power-goal-held` soll den ursprünglichen Typ melden und `resume-after-power`
den wiederaufgenommenen Auftrag. Dann eine Voraussetzung oder den
Upgrade-Träger während des Zwischenauftrags verlieren. `power-goal-released`
soll die Freigabe zur Neuwahl zeigen. Für Nod das Feld `(83,195)` erneut
prüfen. Dazu kommen ein Angriff während der Wartezeit, ein Save während
des Kraftwerksbaus und ein Lauf mit zwei Clients.

## B. DTA Advanced AI v2 und Einheitenproduktion

Untersucht wurde `origin/dta-advaiv2-spawner` im lokalen DTA-Vinifera-Repository
auf `f5ae74aef081f8b1aff11e8d80cd33ef91871974`.

- **NACHGEWIESEN – Auftragsvergabe.** `AdvancedAIUnitProduction` wählt die
  erweiterten Produktionsroutinen. Fahrzeuge prüfen unter anderem MCV- und
  Sammlerbedarf vor der Kampfproduktion. Fahrzeuge, Infanterie, Flugzeuge
  und Marine greifen auf `Get_Team_In_Production` zu. Es filtert nach Haus,
  Teamzustand, Produktionsflags und erlaubter Einheitenkategorie. Die
  Produzenten schreiben in die bestehenden Auftragsfelder. Bodenkampf und
  Infanterie haben zusätzlich einen Start-Rush-Pfad, der auch ohne
  Produktionsteam einen Gegenauftrag auswählen kann.
  [DTA `Get_Team_In_Production` und Fahrzeugproduktion](https://github.com/Rampastring/Vinifera/blob/f5ae74aef081f8b1aff11e8d80cd33ef91871974/src/extensions/house/houseext.cpp#L971) ·
  [DTA Infanterie und Flugzeuge](https://github.com/Rampastring/Vinifera/blob/f5ae74aef081f8b1aff11e8d80cd33ef91871974/src/extensions/house/houseext_hooks.cpp#L2397)
- **NACHGEWIESEN – Feindlage.** `AdvAI_Calculate_Enemy_Strength` iteriert
  `Technos`. Mit festem `house->Enemy` zählt der Code dieses Haus, sonst
  filtert er Verbündete aus. Er erfasst Sammler, Raffinerien und Sensoren,
  summiert Kosten nach Panzerung, Marine und Basisabwehr und zählt
  Primärwaffen ab acht Zellen Reichweite zusätzlich als Artillerie. Getrennte
  Stärkesummen erfassen Wirkung gegen Boden, Luft und Marine. In diesem
  Produktionsscan gibt es keinen Sicht-, Entdeckungs- oder Trefferfilter.
  `AdvAI_Self_Defense_AI` verwendet separat `LATime` und beschädigte
  eigene Gebäude. Eine tatsächliche Feindsichtung wird durch den globalen
  Produktionsscan nicht nachgewiesen.
  [DTA Gegnerstärke](https://github.com/Rampastring/Vinifera/blob/f5ae74aef081f8b1aff11e8d80cd33ef91871974/src/extensions/house/houseext_hooks.cpp#L1257) ·
  [DTA Eigenverteidigung](https://github.com/Rampastring/Vinifera/blob/f5ae74aef081f8b1aff11e8d80cd33ef91871974/src/extensions/house/houseext_hooks.cpp#L1803)
- **NACHGEWIESEN – Waffen und Wirtschaft.** `Weapon_Value` nutzt Schaden,
  Burst, Burst-Verzögerungen, ROF, Reichweite, Warhead-Spread und
  `Verses::Get_Modifier` gegen die Zielpanzerung. Munition, Ungenauigkeit,
  bestimmte Mehrfachtreffer und Spawnwaffen erhalten eigene Gewichtungen.
  `IsAntiGround` und `IsAntiAircraft` gehen in getrennte Werte ein.
  `AdvAI_Get_Object_Value_For_Team` begrenzt Teamkosten und Infanterie,
  gewichtet über- und unterrepräsentierte Rollen und verrechnet unter
  anderem Kosten, Tempo, Haltbarkeit und Tarnung. In den untersuchten
  V2-Kandidatenwertungen ist keine separate ETA bis zur Fertigstellung
  nachgewiesen. ROF und Burst-Verzögerung bewerten die Waffe. Für einen
  späteren ETA-Faktor steht Aftermaths `Time_To_Build`-Pfad der Factory
  als Ausgangspunkt zur Verfügung.
  [DTA `Weapon_Value`](https://github.com/Rampastring/Vinifera/blob/f5ae74aef081f8b1aff11e8d80cd33ef91871974/src/extensions/technotype/technotypeext.cpp#L598) ·
  [DTA Teamwertung](https://github.com/Rampastring/Vinifera/blob/f5ae74aef081f8b1aff11e8d80cd33ef91871974/src/extensions/team/teamext.cpp#L317) ·
  [Aftermath Factory-Zeit, Zeile 112](../src/extensions/factory/factoryext_hooks.cpp#L112)
- **NACHGEWIESEN – Teamverwaltung.** Taktiken erstellen weiterhin Instanzen
  aus `TeamTypes`. Das Team gibt Auftrag, Skript und Rollenmix vor.
  Rekrutierung und Wartung laufen vor der nächsten Taktikentscheidung.
  Angriffstaktiken können Teams zu einem festgelegten Zeitpunkt auslösen
  oder teilen. `AdvAI_Bias_Team_Strength_Ratios_By_Enemy_Strength` mischt
  Feindanteile in die Sollwerte ein. Teamobergrenzen, getrennte
  Produktionskategorien und die Auswahl des jeweils zuständigen Teams
  koordinieren die Aufträge. Ein globaler, unabhängiger Warteschlangenplaner
  ist in diesen Pfaden nicht nachgewiesen.
  [DTA Taktikerzeugung](https://github.com/Rampastring/Vinifera/blob/f5ae74aef081f8b1aff11e8d80cd33ef91871974/src/new/advai/advaitactictype.cpp#L945) ·
  [DTA Taktiken und Rekrutierung](https://github.com/Rampastring/Vinifera/blob/f5ae74aef081f8b1aff11e8d80cd33ef91871974/src/extensions/house/houseext_hooks.cpp#L1978) ·
  [DTA Gegnergewichtung](https://github.com/Rampastring/Vinifera/blob/f5ae74aef081f8b1aff11e8d80cd33ef91871974/src/extensions/house/houseext.cpp#L2017)

**ABGELEITET – Vorschlag für Aftermath.** Zuerst pro KI-Haus eine begrenzte
Feindbeobachtung aus bestätigten Angriffen und synchroner eigener oder
alliierter Aufklärung führen. `MapClass::Sight_From` erhält das Haus und
ist ein Ansatz für die spätere Prüfung. Lokale Werte wie `PlayerPtr` und
`IsDiscoveredByPlayer` sind als gemeinsame Entscheidungsgrundlage ungeeignet.
Für Tarnung kann die bestehende hausbezogene Sensorprüfung mit
`CellClass::Sensed_By` als Vorlage dienen. Gesehene Gegner mit letzter
Position, Typ und Simulationsframe ablegen. Unsichere Informationen
altern lassen. Den Einblick in unsichtbare Bewegung und versteckte
Verluste über globale Objektlisten begrenzen. Die Verfügbarkeit einer
synchronen Sichtabfrage pro KI-Haus bleibt **UNGEKLÄRT**.
[`MapClass::Sight_From`, Zeile 73](../TSpp/src/map.h#L73) ·
[Sensorprüfung, Zeile 3079](../src/extensions/techno/technoext_hooks.cpp#L3079)

Danach je Haus und Kategorie genau einen Produzenten für
`BuildUnit`, `BuildInfantry` und `BuildAircraft` vorsehen. Bestehende
Produkte und rekrutierbare Einheiten auf den Bedarf anrechnen. Fehlende
Sammler und Verteidigung gegen beobachtete Angriffe vor dem Auffüllen eines
Angriffsteams bewerten. Typen über `Can_Build`, Voraussetzungen, BuildLimit,
`BuiltAt` und freie beziehungsweise geeignete Fabriken oder Docks filtern.
Das verfügbare Budget um laufende Zahlungen und die Gebäudereserve kürzen.
Aus Waffeneignung gegen beobachtete Panzerung, verbleibender Rollenlücke,
Kosten und effektiver Bauzeit einen nachvollziehbaren Wert ableiten.
TeamTypes und Skripte können die ausgewählten Einheiten weiter einsetzen.
Feste Simulationsintervalle und Gleichstände nach Kategorie und Typ-ID
halten Entscheidungen auf beiden Clients reproduzierbar. Für eine
zweite Phase Flugabwehr, Luftwaffe und Marine ergänzen und jede Kategorie
gegen den bestehenden Produktionspfad abnehmen.
[Aftermath Fahrzeugauswahl, Zeile 838](../src/extensions/house/houseext.cpp#L838)

## C. Jumpjet-Zielerfassung

- **NACHGEWIESEN – Flugzustand.** `JumpjetLocomotionClass` kennt
  `GROUNDED`, `ASCENDING`, `HOVERING`, `CRUISING` und `DESCENDING`.
  `NewJumpjetLocomotionClass::In_Which_Layer` berechnet den Layer aus
  `HeightAGL` mit einer Brückenkorrektur. Im untersuchten `game.exe`
  verwenden `ObjectClass::In_Air` (`0x005872A0`) und `On_Ground`
  (`0x00587280`) dasselbe Höhengrenzmaß mit gegensätzlichen Vergleichen
  sowie einem Platzierungsflag. Die OpenTS-Rekonstruktion benennt das Maß
  als `LEVEL_LEPTON_H` und zeigt in `FootClass` die Hover-Ausnahme.
  Die exakte native Verzweigung des `FootClass`-Overrides blieb
  **UNGEKLÄRT**. Render-Layer, aktueller Locomotor-Zustand und Zielklasse
  sind getrennte Beobachtungen.
  [Locomotor-Zustände, Zeile 56](../TSpp/src/locomotors/jumpjetlocomotion.h#L56) ·
  [Vinifera-Layer, Zeile 507](../src/new/locomotion/newjumpjetlocomotion.cpp#L507) ·
  [OpenTS `FootClass::In_Air`, Zeile 4766](C:/dev/Aftermath-Engine/references/OpenTS/code/foot.cpp#L4766)
- **NACHGEWIESEN – Zielwahl und Feuerfreigabe.** Aftermaths
  `_Can_Fire` prüft `In_Air` und `On_Ground` gegen die Projektilflags
  und behandelt gelandete Carryalls mit Fracht ausdrücklich. Die
  Waffenwahl entwertet `FIRE_CANT`-Slots, kann bei Gleichstand aber
  trotzdem Primär zurückgeben. `_Evaluate_Object` prüft Maske, Zustand,
  Zone und gegebenenfalls Reichweite, akzeptiert aber einen Luftkandidaten
  ohne abschließende Prüfung, ob überhaupt ein Slot dessen Zieldomäne
  bekämpfen kann. `_Coordinate_Attack` verteilt außerdem das Teamziel
  direkt an geeignete Missionsmitglieder, ohne vor dieser Übergabe die
  Waffendomäne des einzelnen Mitglieds zu prüfen. So kann eine Einheit
  ein Ziel annehmen, das die spätere Feuerprüfung ablehnt.
  [Waffenwahl, Zeile 426](../src/extensions/techno/technoext_hooks.cpp#L426) ·
  [Feuerfreigabe, Zeile 856](../src/extensions/techno/technoext_hooks.cpp#L856) ·
  [Zielbewertung, Zeile 3063](../src/extensions/techno/technoext_hooks.cpp#L3063) ·
  [Teamangriff, Zeile 405](../src/extensions/team/teamext_hooks.cpp#L405)
- **NACHGEWIESEN – Reichweite der bestehenden Hooks.** Move, Guard und
  Guard Area rufen nach `CanPassiveAcquire` die automatische Suche auf.
  `TargetZoneScan` beantwortet die Bewegungskomponente, auch mit der
  Einstellung `InRange`. Der Vergeltungspfad respektiert `CanRetaliate`
  und hat einen speziellen Anti-Air-Test auf `RTTI_AIRCRAFT`.
  `_Target_Something_Nearby` prüft vorhandene Ziele bei Gelegenheitsschüssen
  oder Reichweitensuche erneut. Hunt und Skript-Angriffe müssen deshalb
  zusätzlich zum passiven Guard-Pfad untersucht werden. Die gemeinsame
  automatische Zielwahl ist auch für spielergesteuerte Einheiten
  erreichbar. Welcher dieser Wege die beobachtete Verfolgung ausgelöst
  hat, ist ohne gezielten Lauf **UNGEKLÄRT**. Die DTA-V2-Referenz zeigt
  denselben Luft-/Boden-Check in der Feuerfreigabe und die Layerberechnung,
  aus diesen Stellen ergibt sich keine vollständige Jumpjet-Zielkorrektur.
  [Guard-Hooks, Zeile 479](../src/extensions/foot/footext_hooks.cpp#L479) ·
  [Zielprüfung, Zeile 574](../src/extensions/techno/technoext_hooks.cpp#L574) ·
  [Zielzonen, Zeile 2928](../src/extensions/techno/technoext_hooks.cpp#L2928) ·
  [DTA Feuerfreigabe](https://github.com/Rampastring/Vinifera/blob/f5ae74aef081f8b1aff11e8d80cd33ef91871974/src/extensions/techno/technoext_hooks.cpp#L859)

**ABGELEITET – risikoarmer Eingriff.** Einen seiteneffektfreien
Domänentest für Primär- und Sekundärwaffe mit derselben aktuellen
Luft-/Bodenklassifizierung wie in `_Can_Fire` einführen.
`_Evaluate_Object` (`0x0062D0F0`) ist der erste Hook für Neuziele.
Bei Teamangriffen in `_Coordinate_Attack` (`0x006245B0`) Mitglieder
einzeln prüfen, damit das Teamziel für geeignete Flugabwehr erhalten
bleibt. Anschließend Hunt und bestehende `TarCom`-Ziele bei
Domänenwechsel nachprüfen. Wiederaufladen und fehlende Reichweite bleiben
temporäre Zustände. Medic, Capture, Zellenangriffe und Carryall-Ausnahmen
brauchen eigene Abnahme. Testfälle umfassen Bodenwaffe, reine AA und
gemischte Slots gegen denselben Jumpjet am Boden, beim Start und bei
Landung, jeweils in Guard, Hunt, Vergeltung und Team-Attack. Dazu kommen
Brücken, Hover, der Spieler-Autoangriff und ein gemischtes Team.

## D. Vein-Wegfindung

- **NACHGEWIESEN – tatsächlicher Schaden.** Das untersuchte `game.exe`
  prüft in `CellClass::Trigger_Veins` (`0x0045C5C0`) auf das native
  Overlay `OVERLAY_VEINS` (`126`), `OverlayData >= 0x30`, keine Rampe
  und noch keine Angriffsanimation. `AnimClass::Vein_Attack_AI`
  (`0x004166C0`) arbeitet im geraden Simulationsframe über die
  Zellenbelegung. Der Opferfilter umfasst Technos, Aktivität, positive
  Lebenspunkte, Höhe über Grund unter 6, Typimmunität
  `IsImmuneToVeins` und `Has_Ability(ABILITY_VEIN_PROOF)`.
  Die OpenTS-Fassung benennt die Schadensparameter als `Rule->VeinDamage`
  und `VeinholeWarhead`. Die lokale Rules-Datei enthält derzeit
  `VeinDamage=5`. Betroffen sein können damit unter anderem Infanterie,
  Fahrzeuge und niedrig stehende Technos. Der tatsächlich verursachte
  Schaden hängt auch am geladenen Warhead und der Schadensbehandlung
  des Opfers. `IsVeins` dient unter anderem der Ausbreitungsprüfung.
  `LAND_WEEDS` oder der fehlende INI-Key allein reichen als
  Gefahrenkriterium für die spätere KI-Wegwahl nicht aus.
  [Aftermath Anim-Hook, Zeile 271](../src/extensions/anim/animext_hooks.cpp#L271) ·
  [OpenTS Schadenspfad, Zeile 1450](C:/dev/Aftermath-Engine/references/OpenTS/code/anim.cpp#L1450) ·
  [OpenTS Zellentrigger, Zeile 5873](C:/dev/Aftermath-Engine/references/OpenTS/code/cell.cpp#L5873) ·
  [Aftermath Rules, Zeile 231](<D:/DEVELOPMENT/TS Aftermath/INI/rules.ini:231>)
- **NACHGEWIESEN – nativer Wegeinstieg.** `FootClass::Find_Path`
  (`0x00498EF0`) übergibt das Foot-Objekt an die A*-Suche. Die
  Rekonstruktion zeigt in `FootClass::Basic_Path` die Zonen- und
  Zielpassierbarkeitsprüfung. Die lokale A*-Nachbarschaft bewertet
  `Can_Enter_Cell` und `Get_Movement_Cost` mit der konkreten
  `FootClass`. Aftermath patcht bereits die Puffer und Zonenindizes
  von A*. Der passende native Hook für eine zusätzliche Vein-Kostenkomponente
  ist anhand der Kandidatenkosten und des Foot-Parameters zu verifizieren.
  Seine genaue Binäradresse sowie die Abdeckung aller Drive-, Walk-, Hover-,
  Tunnel- und Sammlerpfade bleiben **UNGEKLÄRT**. Für den Einstieg sind
  reguläre Bodeneinheiten mit nativer A*-Route am besten abgrenzbar.
  [TS++ Wegeinstieg, Zeile 3474](../TSpp/src/tspp/tspp_definitions.cpp#L3474) ·
  [OpenTS Kostenfunktion, Zeile 136](C:/dev/Aftermath-Engine/references/OpenTS/code/astar.cpp#L136) ·
  [OpenTS Kandidatenerweiterung, Zeile 427](C:/dev/Aftermath-Engine/references/OpenTS/code/astar.cpp#L427) ·
  [Aftermath A*-Hooks, Zeile 22](../src/extensions/astar/astarext_hooks.cpp#L22)

**ABGELEITET – vorgeschlagenes Verhalten.** Im nativen Kostenpfad pro
konkreter anfälliger KI-Einheit eine endliche Strafkomponente für eine
gefährliche Zelle einführen. Zonen und generelle Passierbarkeit können
dabei bestehen bleiben. Die Prüfung muss die tatsächlich benutzte Ebene
der Route berücksichtigen, etwa Brückendeck oder Bodenzelle. Auf
`Is_Human_Player`, `Is_Player_Control` und den wirksamen Advanced-AI-Schalter
begrenzen. Ein globaler Overlay-, Zonen- oder `Can_Enter_Cell`-Patch hätte
Auswirkungen auf Spielerbefehle, andere Locomotoren und Kartenanalysen.

| Fall | Vorgeschlagenes Verhalten |
| --- | --- |
| A. Sicherer Weg vorhanden | Gefahr im A*-Kandidatenwert bestrafen. Einen sicheren Umweg bevorzugen, solange die Route für Auftrag und Einheit brauchbar bleibt. |
| B. Nur gefährlicher Weg vorhanden | Weg weiterhin als erreichbar behandeln. Reguläre KI-Aufträge dürfen abwarten oder umplanen, dringende Aufträge erhalten eine begrenzte Risikofreigabe. |
| C. Ziel innerhalb der Veins | Für Angriffe eine sichere Feuerposition in Reichweite prüfen. Bei einem exakten Missionsziel die Kosten beziehungsweise Überlebensfähigkeit beurteilen. |
| D. Veins entstehen unterwegs | Den nächsten geplanten Abschnitt bei relevanter Änderung erneut prüfen. Nur betroffene Einheiten mit einer festen Frame-Sperre umplanen. |
| E. Direkter Angriffsbefehl | Spielergesteuerte Aufträge wie bisher verarbeiten. KI- und Teamziele behalten, bis eine sichere Feuerposition oder eine bewusst erlaubte gefährliche Annäherung feststeht. |

Beim Sammler zuerst die tatsächliche Anfälligkeit prüfen. Tiberiumsammler
sollen ihren Dock- und Ernteauftrag über die sichere Alternative fortsetzen.
Ein Weeder mit gültiger Vein-Immunität kann seine regulären Ziele weiter
erreichen. Hunt, Guard und Teambewegung über denselben nativen Kostenpfad
abdecken, soweit ihre Locomotoren ihn tatsächlich nutzen. Geänderte Routen
und Fallbacks müssen in festen Simulationsframes mit identischen Zellkosten
und stabilen Gleichständen auf allen Clients entstehen. Die Gefahrenprüfung
pro Kandidat kurz halten. Kartenweite Neuberechnungen pro Frame und
wiederholte Pfadsuchen bei jedem kleinen Overlaywechsel vermeiden.

## Prüfstand

Statische Quellcodeprüfung und `git diff --check` ohne Beanstandungen.
Die beiden ungetrackten bearbeiteten Dateien wurden zusätzlich mit
`git diff --no-index --check` geprüft. Build, Ingame-Abnahme und
Zwei-Client-Abgleich stehen aus.
