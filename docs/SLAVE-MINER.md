# Slave Miner – Stufe 1: Gerüst, Suche, Entfalten, Umziehen

Stand: 2026-11-20, Branch `feature/slave-miner`. Kompiliert (RelWithDebInfo), aber noch **nicht im Spiel getestet**.

Hintergrund und YR-Analyse: `../../analysis/SLAVE-MINER-ANALYSE.md`.

## Was Stufe 1 kann

Ein Fahrzeug mit `DeploysInto=` und `SlaveMiner=yes` arbeitet als mobile Raffinerie nach dem Ablauf aus Yuris Rache:

1. **Befehl oder Leerlauf:** Ein menschlicher Spieler wählt den Miner und klickt auf Tiberium (Ernte-Cursor wie beim Harvester). Ohne Befehl bleibt das Fahrzeug stehen. Die KI startet von selbst. Mit `SlaveMinerKickFrameDelay` ≥ 0 starten auch Miner menschlicher Spieler nach so vielen Frames selbst, wenn Tiberium im Nahbereich liegt (Verhalten von YR).
2. **Suche:** Der Bauplatz ist der **nächste gültige** Platz am Feld. Gesucht wird Ring für Ring bis 12 Zellen um das Erz, damit auch Felder zwischen Hängen und Klippen nutzbar sind. Bei gleichem Abstand gewinnt der kürzere Weg. Der eigene Standplatz des Fahrzeugs zählt als frei.
   Zielwahl: Bei einem Befehl das angeklickte Feld (bzw. das nächste Tiberium im Nahbereich darum), sonst eine weite Suche um das Fahrzeug. Dann ein Platz in Gebäudegröße daneben, den das Fahrzeug erreichen kann. Ist am angeklickten Feld kein Platz, fährt der Miner nur hin und wartet.
3. **Fahrt und Entfalten:** über `UnitClass::Try_To_Deploy`. Die Platzierung wird vorher geprüft (keine EVA-Meldung bei Fehlversuch); bei Fehlschlag folgen 30 Frames Pause und ein neuer Versuch, nach drei Fehlschlägen eine neue Suche.
4. **Arbeiten:** Der entfaltete Miner erzeugt **selbst kein Geld**. Einnahmen entstehen wie bei einer Raffinerie nur, wenn Sammler bei ihm entladen; die Sklaven kommen in Stufe 2. In Stufe 1 steht er also nur da und zieht bei Bedarf um.
5. **Umziehen (nur mit Ernteauftrag):** Ist im Nahbereich kein Tiberium mehr, wird weit gesucht. Der Miner packt nur ein, wenn der neue Platz um mehr als `SlaveMinerScanCorrection` näher am Erz liegt, und fährt dann dorthin.

**Ernteauftrag (wie YR):** Den Auftrag bekommt der Miner durch einen Klick auf Tiberium. Bei der KI gilt jeder Start als Auftrag. Nur mit Auftrag zieht der Miner um, wenn das Erz zu weit weg ist. Ein von Hand entfalteter Miner ohne Auftrag bleibt stehen, wo er ist. Jeder andere Befehl beendet den Auftrag, danach bleibt der Miner untätig stehen: ein Fahrbefehl während der Fahrt oder beim Entfalten, Stopp, Angriff oder Einpacken von Hand.

Spielerbefehle haben Vorrang. Verkaufen funktioniert normal. Ein Fahrbefehl mit dem Gebäude (wie beim MCV-Redeploy) packt es ein, danach wartet das Fahrzeug.

Auch das **entfaltete** Gebäude zeigt über Tiberium den Ernte-Cursor. Ein Klick auf ein Feld außerhalb seiner Reichweite packt es ein, es fährt dorthin und entfaltet sich wieder. Liegt das Feld schon in Reichweite, passiert nichts.

Noch **nicht** enthalten: Einnahmen (kommen mit den Sklaven), Sklaven (Infanterie oder Fahrzeuge), KI-Produktion/Zählung als Harvester, Pips, Sounds.

## INI

```ini
[General]
SlaveMinerShortScan=-1        ; Zellen; -1 = TiberiumShortScan. Nahbereich "noch Erz da?" / Wecken.
SlaveMinerLongScan=-1         ; Zellen; -1 = TiberiumLongScan. Suche nach neuem Feld.
SlaveMinerScanCorrection=3    ; Zellen; Umzug nur, wenn der neue Platz um mehr als das näher liegt.
SlaveMinerKickFrameDelay=-1   ; Frames bis ein untätiger Miner eines Menschen von selbst startet; negativ (Standard) = nur auf Befehl.

[SOMEMINER]                   ; VehicleType
DeploysInto=SOMEMINERBASE
SlaveMiner=yes

[SOMEMINERBASE]               ; BuildingType
UndeploysInto=SOMEMINER
SlaveMiner=yes                ; optional, wird über UndeploysInto erkannt
SlaveMiner.HarvestRadius=-1   ; Zellen; Arbeitsradius. Ein Ernteauftrag in diesem Radius löst keinen Umzug aus. -1 = ShortScan
; Optional pro Typ (Fahrzeug oder Gebäude): SlaveMiner.ShortScan, .LongScan,
; .ScanCorrection, .KickFrameDelay – überschreiben die [General]-Werte.
```

Die Einstellungen werden vom aktuellen Typ gelesen; ist nur das Gegenstück (Fahrzeug bzw. Gebäude) als `SlaveMiner=yes` markiert, gelten dessen Werte.

## Technik

| Teil | Ort |
|---|---|
| Logik | `src/new/slavemanager/slavemanager.h/.cpp` (`SlaveManagerClass`, Zustände wie YR `UpdateMiner` 0x6AFD60) |
| Erzeugen/Löschen/Laden | `TechnoClassExtension` (`SlaveManager`-Zeiger, wie `SpawnManager`) |
| Takt | `TechnoClassExt::_Mission_AI`, alle 10 Frames |
| Übergabe beim Entfalten | Hook `0x00651122` (`UnitClass::Try_To_Deploy`, ESI = Fahrzeug, EDI = Gebäude) |
| Übergabe beim Einpacken | Hook `0x00430A01` (`BuildingClass::Mission_Deconstruction`, ESI = Gebäude, EBP = Fahrzeug) |
| Ernte-Cursor Fahrzeug | Hook `0x00656613` (`UnitClass::What_Action`, ersetzt die `IsToHarvest`-Prüfung vor `ACTION_HARVEST`) |
| Ernte-Cursor Gebäude | `BuildingClassExt::_What_Action(Cell)` |
| Ernte-Befehl Gebäude | Hook `0x0042C210` (`BuildingClass::Active_Click_With(Cell)`): `EVENT_ARCHIVE` mit der Zelle, dann `MISSION_HARVEST` |
| Ernte-Befehl auswerten | `SlaveManagerClass::Handle_Harvest_Order` (Fahrzeug: Zelle aus `NavCom`, Gebäude: aus `ArchiveTarget`) |
| Einpacken statt Verkaufen | neuer Hook `0x004306C8`: Slave-Miner-Gebäude mit ArchiveTarget packen ein |
| Speichern/Laden | `SlaveManagers`-Vektor, UUID `BD259ED6-74D4-4D15-894E-75A7BC3823E0`, `EXT_RTTI_SLAVE_MANAGER` |

Hinweise:
- Die Tiberium-Suche ignoriert Shroud absichtlich (Shroud ist pro Spieler verschieden, sonst Desync-Gefahr). Ein Miner kann daher auch in unaufgedecktes Gebiet fahren.
- Speicherstände aus älteren Builds sind nicht kompatibel (neue Klasse, neue Extension-Felder).

## Offene Tests

- Klick mit dem Fahrzeug auf Tiberium: Cursor, Fahrt, Entfalten (kein Geld ohne Sammler). Umziehen nach Erschöpfung.
- Fahrzeug ohne Befehl neben Tiberium: bleibt stehen (Mensch), legt los (KI).
- Fahrbefehl während der Fahrt zum Feld und während des Entfaltens: bleibt am Ziel stehen, entfaltet sich nicht.
- Von Hand entfaltet (ohne Auftrag), Feld leer: zieht nicht um. Mit Auftrag: zieht um.
- Klick mit dem Gebäude auf ein entferntes Feld: Einpacken, Fahrt, Entfalten. Klick in Reichweite: nichts passiert.
- Mehrere Miner gleichzeitig ausgewählt, Klick auf dasselbe Feld.
- Spieler: Fahrbefehl während der Fahrt, Verkaufen, Fahrbefehl auf das Gebäude.
- Speichern/Laden in jedem Zustand (Fahrt, Drehen beim Entfalten, Arbeiten, Einpacken).
- Multiplayer-Sync (zwei Spieler, Miner aktiv).
- Ingenieur übernimmt das entfaltete Gebäude.

## Harvester: Weiterarbeiten nach Dockverlust

Eigenständiger Patch, auch für normale Harvester (`Harvester=yes` bzw. Weed Eater).

- **Problem (TS):** `UnitClass::Mission_Harvest` schaltet einen Harvester auf Guard, wenn sein Haus kein Gebäude aus seiner `Dock=`-Liste mehr hat (0x00654B28). Danach gab ihm niemand mehr einen Ernteauftrag, auch nicht nach dem Bau eines neuen Docks. Verschwand die Raffinerie, während der Harvester auf dem Weg dorthin war, machte `Enter_Idle_Mode` aus dem übrig gebliebenen Fahrziel einen Fahrbefehl (MOVE). Der Harvester fuhr dann zur Stelle, wo die Raffinerie gestanden hatte, und blieb dort für immer stehen.
- **Lösung:** Unabhängig davon, auf welchem Weg TS den Harvester anhält:
  - `UnitClassExtension::IsPlayerOrdered`: Der letzte Befehl kam vom Spieler (Event `MEGAMISSION`/`MEGAMISSION_F` im bestehenden Hook `0x004948A5`, oder Stopp über `EVENT_IDLE`). So ein Harvester setzt nie von selbst fort. Die Markierung endet, sobald er wieder erntet.
  - `IsWaitingForDock`: Hook `0x00654B28` merkt sich, dass `Mission_Harvest` mangels Dock auf Guard geschaltet hat.
  - `UnitClassExtension::Dock_Wait_AI` prüft alle 30 Frames jeden Harvester, der nicht vom Spieler angehalten wurde, nicht in einem Team ist und entweder wartet oder **voll** ist. Ist er untätig (Guard, kein Fahrziel) oder fährt er voll beladen zur alten Dock-Stelle, und hat sein Haus ein **fertiges** Dock (nicht im Bau, nicht beim Verkaufen, nicht in der Seitenleiste), bekommt er wieder den Ernteauftrag. Ein voller Harvester fährt dann direkt zum Dock.
  - Hook `0x00650456` (`Enter_Idle_Mode`): Scheitert die Einfahrt eines beladenen Harvesters, erntet er weiter statt einen Fahrbefehl zur alten Dock-Stelle zu bekommen. Ob TS diesen Weg im Fall „Raffinerie verschwindet auf dem Rückweg“ nimmt, ist nicht bestätigt; die Regel oben deckt den Fall auch ohne ihn ab.
- **Tests:** letzte Raffinerie verkaufen, während der Harvester erntet, auf dem Weg ist oder entlädt. Dann neue Raffinerie bauen: Der Harvester fährt erst los, wenn sie fertig ist. Während des Wartens ein Fahrbefehl: Der Harvester bleibt danach stehen. Speichern und Laden während des Wartens. KI-Harvester.
