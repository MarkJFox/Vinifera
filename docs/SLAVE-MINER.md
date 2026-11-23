# Slave Miner – Stufe 1 und 2: Miner und Infanterie-Sklaven

Stand: 2026-11-21, Branch `feature/slave-miner`. Stufe 1 ist im Spiel getestet. Stufe 2 (Sklaven) kompiliert (RelWithDebInfo), ist aber noch **nicht im Spiel getestet**.

Hintergrund und YR-Analyse: `../../analysis/SLAVE-MINER-ANALYSE.md`.

## Stufe 1: der Miner

Ein Fahrzeug mit `DeploysInto=` und `SlaveMiner=yes` arbeitet als mobile Raffinerie nach dem Ablauf aus Yuris Rache:

1. **Befehl oder Leerlauf:** Ein menschlicher Spieler wählt den Miner und klickt auf Tiberium (Ernte-Cursor wie beim Harvester). Ohne Befehl bleibt das Fahrzeug stehen. Die KI startet von selbst. Mit `SlaveMinerKickFrameDelay` ≥ 0 starten auch Miner menschlicher Spieler nach so vielen Frames selbst, wenn Tiberium im Nahbereich liegt (Verhalten von YR).
2. **Suche:** Der Bauplatz ist der **nächste gültige** Platz am Feld. Gesucht wird Ring für Ring bis 12 Zellen um das Erz, damit auch Felder zwischen Hängen und Klippen nutzbar sind. Bei gleichem Abstand gewinnt der kürzere Weg. Der eigene Standplatz des Fahrzeugs zählt als frei.
   Zielwahl: Bei einem Befehl das angeklickte Feld (bzw. das nächste Tiberium im Nahbereich darum), sonst eine weite Suche um das Fahrzeug. Dann ein Platz in Gebäudegröße daneben, den das Fahrzeug erreichen kann. Vor dem Eingang müssen drei Zellen ohne Gebäude liegen, die mittlere begehbar, und von dort muss das Erz zu Fuß erreichbar sein. Der Platz darf den Eingang eines anderen Slave Miners und die Zellen davor nicht überbauen. Ist am angeklickten Feld kein Platz, fährt der Miner nur hin und wartet.
3. **Fahrt und Entfalten:** über `UnitClass::Try_To_Deploy`. Die Platzierung wird vorher geprüft (keine EVA-Meldung bei Fehlversuch); bei Fehlschlag folgen 30 Frames Pause und ein neuer Versuch, nach drei Fehlschlägen eine neue Suche.
4. **Arbeiten:** Der entfaltete Miner erzeugt **selbst kein Geld**. Einnahmen entstehen nur, wenn seine Sklaven bei ihm abladen (Stufe 2).
5. **Umziehen (nur mit Ernteauftrag):** Liegt im Nahbereich (`ShortScan`) um die Mitte des Miners kein Tiberium mehr, wird weit gesucht. Mit Sklaven zählt dabei nur Tiberium, das sie vom Eingang aus zu Fuß erreichen. Die Sklaven selbst arbeiten weiter in ihrem größeren Radius, bis der Miner einpackt. Nach dem Entfalten prüft der Miner frühestens nach 600 Frames, ob er umzieht. Auch zu kleinen oder nachwachsenden Resten rückt er nach, damit die Sklaven kurze Wege haben. Ist der Eingang zugebaut, zieht er in jedem Fall um, auch wenn der neue Platz nicht näher am Erz liegt. Der Miner packt nur ein, wenn der neue Platz um mehr als `SlaveMinerScanCorrection` näher am Erz liegt, und fährt dann dorthin.

**Ernteauftrag (wie YR):** Den Auftrag bekommt der Miner durch einen Klick auf Tiberium. Bei der KI gilt jeder Start als Auftrag. Nur mit Auftrag zieht der Miner um, wenn das Erz zu weit weg ist. Ein von Hand entfalteter Miner ohne Auftrag bleibt stehen, wo er ist. Jeder andere Befehl beendet den Auftrag, danach bleibt der Miner untätig stehen: ein Fahrbefehl während der Fahrt oder beim Entfalten, Stopp, Angriff oder Einpacken von Hand.

**Reparieren:** TS sperrt die Reparatur für Gebäude mit `UndeploysInto=` (wie den Tick Tank), weil sie als Fahrzeug gelten. Ein entfalteter Slave Miner lässt sich trotzdem wie ein normales Gebäude reparieren, solange `SlaveMiner.Repairable=yes` (Standard) gilt. `Repairable=no` des Gebäudetyps sperrt weiterhin.

Spielerbefehle haben Vorrang. Verkaufen funktioniert normal. Ein Fahrbefehl mit dem Gebäude (wie beim MCV-Redeploy) packt es ein, danach wartet das Fahrzeug.

Auch das **entfaltete** Gebäude zeigt über Tiberium den Ernte-Cursor. Ein Klick auf ein Feld außerhalb seiner Reichweite packt es ein, es fährt dorthin und entfaltet sich wieder. Liegt das Feld schon in Reichweite, passiert nichts.

## Stufe 2: die Sklaven

Hat der Miner `SlaveMiner.Slave=` (Infanterietyp), erzeugt er beim ersten Logik-Takt `SlaveMiner.SlaveCount` Sklaven. Sie sitzen im Miner und fahren mit ihm mit. Jeder Sklave arbeitet so (Ablauf wie YR `UpdateSlaves`):

1. **Aussteigen:** Nur wenn der Miner entfaltet ist und arbeitet. Außerdem muss Tiberium um den Eingang liegen, im Radius `SlaveScan` oder `SlaveMiner.HarvestRadius`, je nachdem, welcher größer ist. Oder der Miner kennt noch ein Erzfeld (siehe Schritt 2). Der Eingang ist die mittlere Zelle der rechten Gebäudekante. Sind dort schon drei Infanteristen, oder ist sie belegt, steigt der Sklave auf eine Zelle direkt rechts davor aus.
2. **Suchen und Hinlaufen:** Der Sklave sucht das nächste Tiberium um sich selbst und läuft hin. Gesucht wird nur Tiberium, das er zu Fuß erreichen kann (gleiche Bewegungszone). Bleibt er trotzdem vor dem Ziel stehen, weil kein Weg hinführt, oder kommt er 150 Frames lang nicht näher (Stau mit anderer Infanterie), überspringt er diese Zelle und ihre Nachbarn bei den folgenden Suchen, bis er wieder etwas geerntet hat oder der Miner umgezogen ist. Findet er in seiner Reichweite keins und zieht der Miner nicht selbst um (menschlicher Spieler ohne Ernteauftrag), läuft er zum gemerkten Erzfeld des Miners und sucht dort weiter. Gemerkt wird die Zelle, an der zuletzt ein Sklave geerntet hat, sonst das Ziel des letzten Ernteauftrags oder Umzugs. Ist dieses Feld abgeerntet, gilt Tiberium in `SlaveScan` um das Feld. Gibt es dort auch keins, sucht der Miner höchstens alle 450 Frames im Radius `LongScan` um sich selbst. Findet er nichts, bleiben die Sklaven drin.
3. **Abbauen:** Alle `HarvestRate` Frames nimmt er eine Einheit von der Zelle, bis er voll ist (`Storage=` des Infanterietyps). Ist die Zelle leer, sucht er weiter. Findet er nichts, geht er zurück.
4. **Zurückbringen:** Er läuft zum Eingang und **betritt das Gebäude**: Er verschwindet, sobald er die Eingangszelle betritt. Dabei gibt er seinen Infanterie-Platz in der Zelle frei; TS macht das beim Entfernen von Infanterie nicht selbst, sonst wäre der Eingang nach drei Sklaven dauerhaft voll. Ein laufender Sklave verliert dabei auch seinen angefangenen Schritt, damit er nach dem Aussteigen nicht den alten Schritt zu Ende läuft und seinen Ausstiegsplatz belegt lässt. Erst dabei wird seine Ladung dem Haus gutgeschrieben, genau wie beim Entladen eines Harvesters an der Raffinerie. Ins eingepackte Fahrzeug steigt kein Sklave ein. Volle Sklaven halten Abstand zu der Fläche, die das Gebäude beim Entfalten belegen wird. Fährt das Fahrzeug zu einem Bauplatz, gilt dessen Fläche, sonst die am aktuellen Standort. Näher als 1 Zelle treten sie zur Seite, bis 3 Zellen bleiben sie stehen, ab mehr als 5 Zellen laufen sie nach. Nach dem Entfalten gehen sie zum Eingang. Kommt ein Sklave auf dem Rückweg 75 Frames lang nicht näher an den Eingang, sucht er einen neuen Weg. Steht er dabei am Gebäude, tritt er von dort aus ein. Direkt neben dem Eingang tritt er sofort ein, wenn er steht, und nach 30 Frames, wenn er noch läuft, aber nicht hineinkommt (Eingang blockiert). Nach dem Entfalten oder Einpacken beginnt diese Zählung neu. Bleibt er dreimal hintereinander so hängen und steht höchstens 3 Zellen vom Gebäude entfernt, betritt er es von dort aus. Findet die Wegsuche dreimal hintereinander keinen Weg zum Eingang (etwa weil ein anderes Gebäude ihn zubaut), tritt er ein, wenn er höchstens `SlaveScan` vom Gebäude entfernt ist. Sklaven, auf deren Zelle das entfaltete Gebäude steht, sind sofort drin und liefern ihre Ladung ab.
5. **Nachladen:** Er bleibt `ReloadRate` Frames im Miner und wird dabei voll geheilt. Danach steigt er wieder aus.
6. **Tod:** Ein toter Sklave wird nach `RegenRate` Frames kostenlos im Miner ersetzt.

**Spielerbefehle:** Ein Sklave gehorcht jedem Befehl. Sobald er danach untätig ist, arbeitet er von selbst weiter. Die Ladung behält er dabei.

**Miner zerstört oder verkauft:** Sklaven im Miner sterben mit ihm. Sklaven draußen bleiben als normale Infanterie beim Haus.

Hinweise für die INI:
- TS-Tiberium verletzt Infanterie. Sklaven sollten daher `TiberiumProof=yes` haben.
- Die Sklaven sollten nicht baubar sein (keine `Prerequisite`/`TechLevel`), sonst baut der Spieler sie zusätzlich.

Noch **nicht** enthalten: Fahrzeug-Sklaven, Schürf-Animation, KI-Produktion/Zählung als Harvester, Befreiung durch den Angreifer (YR), Pips, Sounds.

## INI

```ini
[General]
SlaveMinerShortScan=-1        ; Zellen; -1 = TiberiumShortScan. Nahbereich "noch Erz da?" / Wecken.
SlaveMinerLongScan=-1         ; Zellen; -1 = TiberiumLongScan. Suche nach neuem Feld.
SlaveMinerScanCorrection=3    ; Zellen; Umzug nur, wenn der neue Platz um mehr als das näher liegt.
SlaveMinerKickFrameDelay=-1   ; Frames bis ein untätiger Miner eines Menschen von selbst startet; negativ (Standard) = nur auf Befehl.
SlaveMinerSlaveScan=-1        ; Zellen; -1 = 14 (wie YR). Suchradius der Sklaven.
SlaveMinerHarvestRate=30      ; Frames pro abgebauter Einheit, wenn der Sklaventyp nichts angibt.

[SOMEMINER]                   ; VehicleType
DeploysInto=SOMEMINERBASE
SlaveMiner=yes
SlaveMiner.Slave=SOMESLAVE    ; InfantryType der Sklaven (ohne: keine Sklaven, kein Einkommen)
SlaveMiner.SlaveCount=5       ; Anzahl Sklaven
SlaveMiner.RegenRate=500      ; Frames bis ein toter Sklave ersetzt wird
SlaveMiner.ReloadRate=25      ; Frames, die ein Sklave nach dem Abladen im Miner bleibt (wird dabei geheilt)
SlaveMiner.SlaveScan=-1       ; Zellen; -1 = [General] SlaveMinerSlaveScan

[SOMEMINERBASE]               ; BuildingType
UndeploysInto=SOMEMINER
SlaveMiner=yes                ; optional, wird über UndeploysInto erkannt
SlaveMiner.HarvestRadius=-1   ; Zellen; Arbeitsradius. Ein Ernteauftrag in diesem Radius löst keinen Umzug aus. -1 = ShortScan
SlaveMiner.Repairable=yes     ; entfaltetes Gebäude per Reparatur-Cursor reparierbar (TS sperrt das sonst für Einpack-Gebäude)
; Optional pro Typ (Fahrzeug oder Gebäude): SlaveMiner.ShortScan, .LongScan,
; .ScanCorrection, .KickFrameDelay – überschreiben die [General]-Werte.

[SOMESLAVE]                   ; InfantryType, Platzhalter
Storage=4                     ; Ladung pro Gang (Einheiten, mindestens 1)
TiberiumProof=yes             ; sonst verletzt das Tiberium den Sklaven
SlaveMiner.HarvestRate=-1     ; Frames pro Einheit; -1 = [General] SlaveMinerHarvestRate
```

Die Sklaven-Einstellungen dürfen am Fahrzeug oder am Gebäude stehen; es gilt der Typ, der `SlaveMiner.Slave` nennt.

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
| Sklaven-Automat | `SlaveManagerClass::Slave_AI` (Zustände wie YR `UpdateSlaves` 0x6AF6C0, dazu `Ordered`), im selben 10-Frame-Takt |
| Eingang betreten | Hook `0x004D5219` (`InfantryClass::Can_Enter_Cell` nach dem Prolog): eigener Sklave darf die Eingangszelle betreten (`MOVE_OK`). Wegsuche und Laufsteuerung nutzen beide diese Funktion; Gebäude beeinflussen die Bewegungszonen in TS nicht. |
| Spielerbefehl an Sklaven | `SlaveManagerClass::Player_Order` aus dem Event-Hook `0x004948A5` und `EVENT_IDLE` |
| Gutschrift | `HouseClass::Tiberium_Harvested` je Tiberiumart, wie beim Harvester |
| Reparieren | Hook `0x00436B78` (`BuildingClass::Can_Repair` nach `Considered_Vehicle`): Slave-Miner-Gebäude mit `SlaveMiner.Repairable` gehen zur normalen Prüfung `TechnoClass::Can_Repair` weiter |
| Speichern/Laden | `SlaveManagers`-Vektor, UUID `BD259ED6-74D4-4D15-894E-75A7BC3823E0`, `EXT_RTTI_SLAVE_MANAGER` |

Hinweise:
- Die Tiberium-Suche ignoriert Shroud absichtlich (Shroud ist pro Spieler verschieden, sonst Desync-Gefahr). Ein Miner kann daher auch in unaufgedecktes Gebiet fahren.
- Speicherstände aus älteren Builds sind nicht kompatibel (neue Klasse, neue Extension-Felder).

## Offene Tests

Stufe 2:
- Miner mit `SlaveMiner.Slave` entfalten: Sklaven steigen am Eingang aus, bauen ab, laufen ins Gebäude, Geld kommt erst beim Betreten.
- Geld pro Gang prüfen (`Storage` × Wert der Tiberiumart).
- Sklave töten: Ersatz nach `RegenRate` im Miner.
- Miner zieht um, während Sklaven draußen sind: Sie folgen dem Fahrzeug mit Abstand, steigen nicht ein und betreten nach dem Entfalten das neue Gebäude. Kein Sklave fehlt danach.
- Sklave betritt den Eingang: Er verschwindet an der Gebäudekante, ohne vorher wieder herauszulaufen.
- Sklave per Hand wegschicken: Er bleibt am Ziel kurz stehen und arbeitet dann weiter.
- Miner zerstören: Sklaven draußen bleiben, die im Miner sterben.
- Andere Infanterie darf die Eingangszelle nicht betreten.
- Speichern/Laden mit Sklaven draußen und drinnen; Multiplayer-Sync.

Stufe 1:
- Entfalteten Miner beschädigen und mit dem Reparatur-Cursor reparieren; mit `SlaveMiner.Repairable=no` gesperrt. Tick Tank bleibt unreparierbar.

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

## Test-Log

Im Entwicklermodus (`-DEVELOPER`) schreibt jeder Slave Miner seine Abläufe ins normale Debug-Log (`Debug\DEBUG_*.LOG`). Ohne Entwicklermodus wird nichts geschrieben. Das Log liest nur den Spielzustand und ändert nichts am Ablauf.

Jede Zeile beginnt mit `[SlaveMiner]` und hat das Format `Frame | Miner | Sklave | Ereignis | Details`. Miner und Sklaven sind durchnummeriert (`M0`, `S3`). Sklaven-Zeilen enden mit Zelle, Ziel, Eingang, `nav` (Laufbefehl), `moving`, `limbo` (im Miner), Mission, Ladung, Lebenspunkten und `stuck` (Hängezähler).

| Ereignis | Bedeutung |
|---|---|
| `owner` | Der Miner wurde zu Fahrzeug oder Gebäude (mit Typ und Zelle). |
| `miner status` | Zustandswechsel des Miners (Idle, Scanning, Travelling, Working, PackingUp ...). |
| `release` | Ob Sklaven aussteigen dürfen, und wenn nicht, warum. Nur bei Änderung. |
| `status` | Zustandswechsel eines Sklaven. |
| `seek` | Gefundenes Erz und woher (um den Sklaven, um den Eingang, gemerktes Feld) oder „nichts gefunden“. |
| `exit` | Aussteigen gelungen oder gescheitert. |
| `enter` | Eintreten mit Grund: auf den Miner getreten, neben dem Eingang stehend, neben dem Eingang blockiert, am Gebäude hängengeblieben, mehrfach hängengeblieben, Gebäude auf ihm entfaltet. |
| `cell` | Belegung des Eingangs und der Zellen davor, bei blockiertem Eingang, beim Hängenbleiben bis 2 Zellen vor dem Eingang und bei gescheitertem Aussteigen (höchstens alle 30 Frames je Miner). `spots` sind die fünf Infanterie-Plätze Mitte, NW, NE, SW, SE (1 = belegt); dazu Fahrzeug-, Hindernis- und Gebäude-Markierung, Gebäudetyp, Anzahl Infanterie und anderer Objekte, Höhe und Rampe. |
| `stuck` | Zurück: 75 Frames ohne Annäherung an den Eingang, mit Abstand zu Eingang und Gebäude. Hinweg: 150 Frames ohne Annäherung an das Erzziel, das Ziel wird übersprungen. |
| `wait` | Warten am Fahrzeug: zur Seite treten, anhalten, nachlaufen. |
| `unreachable` | Ein Erzziel oder der Eingang ist zu Fuß nicht erreichbar (mit Zähler und Abstand zum Gebäude). |
| `relocate check` | Im Nahbereich des Miners liegt kein erreichbares Tiberium mehr oder der Eingang ist zugebaut; der Miner sucht einen neuen Platz. Ob er wirklich umzieht, zeigen die folgenden `miner status`-Zeilen (`PackingUp`). |
| `relocate` | Ergebnis des Umzugsversuchs: `moving to` (packt ein), `no deploy site` (kein passender Platz am Erz) oder `not closer` (neuer Platz nicht um mehr als `ScanCorrection` näher, mit beiden Entfernungen in Zellen). Davor steht `closer sites rejected`: wie viele Plätze näher am Erz verworfen wurden und warum (`zone` Fahrzeug kommt nicht hin, `tiberium`/`slope`/`uneven`/`occupied` Gebäude kann dort nicht stehen, `placement` anderer Grund, `door building`/`door blocked`/`door zone` Eingang zugebaut, nicht begehbar oder ohne Weg zum Erz, `other door` würde den Eingang eines anderen Miners überbauen). |
| `travel` | Der Zielplatz wurde unterwegs belegt und das Spiel hat das Fahrziel auf eine Nachbarzelle verlegt; der Miner sucht neu, statt abzubrechen. |
| `player order` | Der Spieler hat dem Sklaven einen Befehl gegeben. |
| `lost` | Sklave getötet, oder entfernt (gelöscht oder Spielende). |
| `overview` | Alle 150 Frames je Miner eine Übersicht mit allen Sklaven. |

Ein hängender Sklave zeigt sich in den `overview`-Zeilen als gleiche Zelle über mehrere Einträge, meist mit `nav=1 moving=0`.
