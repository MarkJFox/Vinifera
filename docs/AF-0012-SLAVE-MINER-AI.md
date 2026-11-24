# AF-0012 – Slave Miner für die Advanced AI

Stand: 2026-11-24, Branch `feature/af-0012-ai-slave-miner` (von `feature/af-0011-ai-mobile-war-factory`). Kompiliert (RelWithDebInfo), **im Spiel nicht getestet**.

## Ausgangslage

Ein Slave Miner ist ein Fahrzeug mit `SlaveMiner=yes`, das sich in ein Gebäude entfaltet (Aftermath: `TIBRIG` → `GATIBRIG`). Seine Sklaven ernten das Tiberium rund um das Gebäude.

Der Slave-Miner-Code (`SlaveManagerClass`) steuert einen Miner der KI schon selbst. Ein untätiger Miner sucht Tiberium, fährt hin, entfaltet sich und zieht um, wenn das Feld leer ist.

Gefehlt hat:

- **Bauen:** Die Advanced AI kauft als Sammler nur Typen aus `HarvesterUnit=`. `TIBRIG` dort einzutragen hilft nicht: Bei jedem Ersatz nimmt die KI den ersten baubaren Typ der Liste, also immer oder nie den Slave Miner.
- **Ziel:** Ohne Auftrag sucht der Miner das nächste Tiberium. Das liegt direkt an der Basis und wird von den Raffinerien schon abgeerntet.

## Zwei Betriebsarten

Der Code erkennt die Betriebsart an der INI:

| INI | Betriebsart |
|---|---|
| `SlaveMiner=yes` + `SlaveMiner.Slave=…` (am Fahrzeug oder am Gebäude) | **Sklaven-Miner**: Die Sklaven ernten und liefern direkt ab. `Refinery=yes` ist dafür nicht nötig. |
| `SlaveMiner=yes`, kein `SlaveMiner.Slave=`, Gebäude mit `Refinery=yes` | **Mobile Raffinerie**: Normale Sammler laden dort ab. |
| `SlaveMiner=yes`, weder `SlaveMiner.Slave=` noch `Refinery=yes` | Entfaltet sich, bringt aber nichts ein. |

`SlaveMiner.SlaveCount` (Standard 5) gilt nur, wenn ein Sklaventyp gesetzt ist. Den Selbstabbau `SlaveMiner.HarvestAmount` aus den INI-Kommentaren gibt es im Code nicht.

**Sammler für die mobile Raffinerie:** TS-Sammler ernten dort, wo sie gerade sind, und laden bei der nächstgelegenen Raffinerie ab. Sammler aus der Waffenfabrik bleiben also an der Basis. Deshalb ordnet die KI jeder entfalteten mobilen Raffinerie feste Sammler zu:

- **Nur passende Sammler:** Es zählen nur Sammler, deren Typ dort andocken kann (`Dock=` des Sammlers; eine leere Liste gilt als „jede Raffinerie“). In Aftermath ist das `LOCUST` (`Dock=GATIBRIG,GATIBRIG2`); `HARV` (`Dock=PROC`) wird nie zugeordnet.
- Ein Sammler gehört zu ihr, wenn er dort andocken kann und im Umkreis von 10 Zellen steht, dorthin fährt oder dort zuletzt geerntet hat.
- Ein Sammler, der nur an mobilen Raffinerien abladen kann (`LOCUST`) und schon erntet, andockt oder entlädt, gehört zur nächsten passenden mobilen Raffinerie, egal wo er erntet. Er lädt ohnehin nur dort ab. Vorher wurde er alle paar Sekunden neu geschickt, wenn er weiter als 10 Zellen entfernt erntete. Ein neuer oder untätiger `LOCUST` wird einmal geschickt.
- Fehlen Sammler, schickt die KI alle zwei Sekunden höchstens einen freien, passenden Sammler pro Raffinerie dorthin, den nächstgelegenen, mit einem Ernteauftrag auf das Tiberium neben ihr. Danach erntet er dort und lädt dort ab, weil sie die nächste Raffinerie ist.
- Frei ist ein Sammler, der zu keiner mobilen Raffinerie gehört, in keinem Team ist und nicht gerade andockt oder entlädt. Jede feste Raffinerie behält mindestens einen freien Sammler, der bei ihr andocken kann. Sammler, die bei keiner festen Raffinerie andocken können, dürfen alle gehen.
- Anzahl: `AdvancedAIMobileRefineryHarvesters`, Standard -1 = `AdvancedAIHarvestersPerRefinery`. Der `FreeUnit=`-Sammler zählt mit.
- **Sammler-Bedarf:** Die KI berechnet den Bedarf je Sammlertyp nur für die Raffinerien, an denen dieser Typ andocken kann, und zählt nur passende Sammler. Für `PROC` baut sie also weiter `HARV`, für die mobile Raffinerie den nächsten baubaren Typ aus `HarvesterUnit=`, der dort andocken kann (`LOCUST`). Log: `harvester-replacement=LOCUST … (other dock)`.
- Nur während sie entfaltet ist, nicht während des Auf- oder Abbaus. Es wird nichts gespeichert; wer wohin gehört, liest die KI jedes Mal aus der Karte.

## Ablauf

**Bauen** (in `AdvAI_AI_Unit`, direkt nach der mobilen Waffenfabrik). Alle Bedingungen müssen gelten:

- `AdvancedAISlaveMiner=yes`, `AdvancedAISlaveMinerMax` > 0, und die Spielzeit liegt über `AdvancedAISlaveMinerDelay`.
- Seit dem letzten Verlust ist `AdvancedAISlaveMinerRebuildDelay` vergangen.
- Keine Bedrohung durch einen frühen Ansturm, und die KI wurde in der letzten Minute nicht angegriffen.
- Die KI hat eine fertige Waffenfabrik (sonst Grund `no-war-factory`; vorher wurde der Auftrag ohne Fabrik ständig neu erteilt).
- Die KI hat weniger als `AdvancedAISlaveMinerMax` Slave Miner (Fahrzeuge, entfaltete Gebäude und in Produktion zusammen). Die Grenze gilt auch beim Produktionsstart in jeder einzelnen Waffenfabrik (Grund `slave-miner-limit` in `AF-0009-UNIT-A`). Vorher startete ein Auftrag bei mehreren Waffenfabriken zweimal.
- Es gibt ein passendes Feld (siehe unten).
- Der Typ ist für die KI baubar: TechLevel, Besitzer und Voraussetzungen (`TIBRIG`: `NATECH`).
- Die normale Budgetprüfung lässt den Start zu, und danach bleiben mindestens `AdvancedAISlaveMinerReserve` Credits übrig.

Sammler haben Vorrang: Fehlt der KI ein normaler Sammler, baut sie zuerst diesen.

**Passendes Feld:** Grundlage ist die Tiberium-Analyse der Advanced AI (die `AF-0009B2`-Zeilen im Log). Ein Feld muss:

- von der Basis aus erreichbar sein,
- einen erreichbaren Wert von mindestens `AdvancedAISlaveMinerFieldValue` haben,
- mindestens `AdvancedAISlaveMinerRefineryDistance` Zellen von der nächsten eigenen Raffinerie entfernt sein (ohne Raffinerie zählt jedes Feld),
- mindestens `AdvancedAISlaveMinerEnemyDistance` Zellen von der nächsten gegnerischen Basis entfernt sein,
- frei sein: Kein anderer Slave Miner des Hauses steht, entfaltet sich oder ist unterwegs im Umkreis von 12 Zellen,
- keine gegnerische Raffinerie im Umkreis von 12 Zellen haben, auch keine entfaltete mobile (Grund `enemy-refinery`). Vorher zog ein Miner neben die GATIBRIG des Gegners, weil nur der Abstand zu gegnerischen Basen zählte,
- für einen Miner, der schon einmal gearbeitet hat, höchstens `AdvancedAISlaveMinerMaxTravel` Zellen von ihm entfernt liegen (Grund `too-far`).

Von den passenden Feldern gewinnt das mit dem höchsten Wert im Verhältnis zur Entfernung. Für einen neuen Auftrag zählt die Entfernung von der Basis, für einen vorhandenen Miner (etwa nach einem gescheiterten Umzug) die Entfernung vom Miner selbst. Ein frisch gebauter Miner (noch nie aufgebaut) gilt als neuer Auftrag: Er fährt ohne `MaxTravel`-Grenze zu dem Feld, für das er gebaut wurde. Vorher fuhr ein Miner nach einem gescheiterten Umzug bis zu 84 Zellen quer über die Karte und wurde unterwegs zerstört.

**Losschicken:** Wacht ein Slave Miner der KI auf (fertig gebaut, nach einem Umzug ohne Ziel oder nach einem Fehlschlag), fragt der Slave-Miner-Code die KI nach einem Feld. Liegt es mehr als 10 Zellen entfernt, bekommt der Miner es wie einen Ernteauftrag des Spielers. Danach übernimmt der Slave-Miner-Code wie bisher: Bauplatz suchen, hinfahren, entfalten, später umziehen. Ohne passendes Feld oder in der Nähe sucht der Miner wie bisher selbst (Log `scans nearby (…)` mit dem Grund).

**Absicherung im Slave-Miner-Code** (nach dem ersten Test):

- Ein Miner der KI wird erst geweckt, wenn er nicht mehr auf einem Gebäude steht. Sonst stören die Ausfahrbefehle der Waffenfabrik die erste Fahrt.
- Verlässt der Miner beim Entfalten seinen Platz oder dauert das Entfalten länger als 15 Sekunden, gilt der Versuch als gescheitert, und das Entfalten-Kennzeichen wird zurückgesetzt. Vorher blieb der Miner dauerhaft auf „Deploying“ und wurde nie wieder geweckt.
- Bei der KI kommt der Miner danach zurück auf Guard und sucht erneut, höchstens dreimal hintereinander. Dasselbe gilt, wenn das Spiel während der Fahrt die Mission wechselt und wenn ein unbewaffneter Miner auf Hunt steht.
- Die KI schickt den Miner nicht zur Feldmitte (dort ist alles Tiberium, und darauf kann man nicht bauen), sondern zum Feldrand auf seiner Seite. Findet der Slave-Miner-Code um eine Tiberiumzelle keinen Bauplatz, sucht er am Feldrand Richtung Fahrzeug weiter, oder Richtung Basis, wenn der Miner selbst im Feld steht. Das gilt auch für Miner des Spielers. Log: `scan | no site at …, field edge … -> site …`.
- Der Slave-Miner-Code verliert sein Fahrzeug nicht mehr, wenn das Spiel es nur kurz abmeldet (Limbo, nicht mehr anvisierbar). Vorher stand ein solcher Miner danach für immer still, ohne Log-Zeilen.
- Das Slave-Miner-Log (`[SlaveMiner]`) schreibt dazu `travel | mission N … cancelled`, `deploy | aborted (left site/timeout), mission N …` und `idle | computer miner on hunt`.

**Sonstiges:**

- Slave Miner zählen nicht als Sammler; die Raffinerie-Wirtschaft läuft unverändert.
- Sie sind von der Team-Budgetprüfung und der Sparphase ausgenommen, wie Sammler.
- Der Fahrzeug-Ansturm ohne Bauhof und Fabrik schickt sie nicht auf Hunt.
- Teams nehmen sie nicht auf (sie sind unbewaffnet).

## INI

```ini
[AI]
AdvancedAISlaveMiner=yes                 ; KI baut und nutzt Slave Miner
AdvancedAISlaveMinerDelay=7200           ; Frames bis zum ersten (Standard 8 Minuten)
AdvancedAISlaveMinerRebuildDelay=2700    ; Frames nach einem Verlust (Standard 3 Minuten)
AdvancedAISlaveMinerMax=2                ; höchstens so viele pro Haus, 0-10 (0 = nie bauen)
AdvancedAISlaveMinerReserve=1500         ; Credits, die nach dem Kauf übrig bleiben müssen
AdvancedAISlaveMinerFieldValue=10000     ; Mindestwert eines Feldes (wie "reachable-value" im Log)
AdvancedAISlaveMinerRefineryDistance=15  ; Zellen Abstand zur nächsten eigenen Raffinerie, 0-100
AdvancedAISlaveMinerEnemyDistance=25     ; Zellen Abstand zur nächsten gegnerischen Basis, 0-100
AdvancedAISlaveMinerMaxTravel=40         ; so weit fährt ein vorhandener Miner zu einem neuen Feld, 0-200 (0 = beliebig)
AdvancedAIMobileRefineryHarvesters=-1    ; Sammler pro mobiler Raffinerie, 0-10; -1 = AdvancedAIHarvestersPerRefinery
```

Die Standardwerte im Code sind `TICKS_PER_MINUTE`-Vielfache. Die Zahlen oben gelten für 15 Frames pro Sekunde.

Mit `AdvancedAISlaveMiner=no` schickt die KI auch vorhandene Miner nicht mehr gezielt los; sie suchen dann wie bisher selbst.

## Diagnose

Im Entwicklermodus schreibt das Debug-Log Zeilen mit `AF-0012:`:

| Ereignis | Bedeutung |
|---|---|
| `slave-miner ordered` | Bauauftrag erteilt, mit Feld und Anzahl |
| `slave-miner ... sent to` | Miner zu einem Feld geschickt (bei vorhandenen Minern mit `travel=`) |
| `slave-miner ... scans nearby (…)` | Kein Feld für diesen Miner; er sucht selbst in der Nähe |
| `mobile-refinery … harvesters n/m: sent …` | Sammler zur mobilen Raffinerie geschickt (n = vorher dort, m = gewünscht) |
| `sent … for x,y at x,y counts …` | Ein entsandter Sammler wird zwei Minuten beobachtet; bei jeder Änderung steht, wo er zählt: `near` (arbeitet bei einer mobilen Raffinerie), `nearest` (erntet woanders, zählt für die nächste), `free`, `busy`, `limbo` oder `team`, mit Mission, NavCom und ArchiveTarget |
| `sent harvester for x,y gone` | Der beobachtete Sammler ist zerstört oder nicht mehr aktiv |
| `slave-miner lost` | Weniger Miner als beim letzten Durchlauf, Wiederaufbau-Sperre beginnt |
| `slave-miner skip reason=…` | Warum kein Bauauftrag kommt; bei Änderung, höchstens einmal pro Minute je Haus |

Gründe bei `skip`: `delay`, `rebuild-delay`, `start-rush-threat`, `recently-attacked`, `limit (n/max)`, `no-war-factory`, `no-resource-analysis`, `no-field` (mit Zählern: zu arm, zu nah an einer Raffinerie, zu nah am Gegner, schon belegt, gegnerische Raffinerie, zu weit), `cannot-produce` (Besitzer oder Voraussetzungen), `budget`, `money`, `techlevel` (nur wenn kein Typ im Spiel baubar ist), `no-slave-miner-type`. Der Grund stammt vom ersten Typ, der in diesem Spiel überhaupt baubar wäre; `TIBRIG2` mit `TechLevel=-1` verdeckt also nicht mehr den Grund von `TIBRIG`.

## Bekannte Grenzen

- Gefahr wird nur über den Abstand zu gegnerischen Basen gemessen, nicht über Einheiten auf dem Weg.
- Kommt ein Miner nicht an sein Feld (Weg blockiert), versucht er es nach jedem Aufwachen erneut.
- Ein eigenes Begleitschutz-Team gibt es nicht.
- Mobile Raffinerie: Der Umzug richtet sich noch nach dem kleinen Umkreis um das Gebäude (wie beim Sklaven-Miner), nicht nach dem Ernteumkreis der Sammler. Die Sammler fahren beim Umzug nicht mit, sondern werden erst nach dem Entfalten am neuen Platz wieder zugeordnet.
- Ein Sammler, der voll ist und weiter weg erntet, kann trotzdem zu einer anderen Raffinerie fahren, wenn die näher liegt.

## Offene Tests

- Skirmish Nod mit Technologiezentrum: Bauauftrag nach etwa 8 Minuten, Fahrt zu einem entfernten Feld, Entfalten, Sklaven ernten.
- Zweiter Miner geht zu einem anderen Feld.
- Feld leer: Umzug zum nächsten Feld.
- Miner zerstören: Neubau erst nach der Wiederaufbau-Sperre.
- Speichern und Laden, Multiplayer-Sync.

## Dateien

| Datei | Inhalt |
|---|---|
| `src/extensions/house/houseext_advai_slaveminer.cpp` | Bauen, Feldwahl, Verlusterkennung, Sammler der mobilen Raffinerie |
| `src/new/slavemanager/slavemanager.cpp/.h` | Beim Aufwachen das Feld der KI übernehmen, Absicherung beim Entfalten |
| `src/extensions/house/houseext.h/.cpp` | Zustandsfelder (werden mitgespeichert), CRC |
| `src/extensions/house/houseext_advai_units.cpp` | Bauauftrag in `AdvAI_AI_Unit`, Aufruf in `AdvAI_Expert_AI`, Ausnahme beim Fahrzeug-Ansturm |
| `src/extensions/house/houseext_advancedai.cpp` | Ausnahmen bei Team-Budget und Sparphase |
| `src/extensions/rules/rulesext.h/.cpp` | INI-Schlüssel |

Speicherstände aus älteren Builds sind nicht kompatibel (neue Felder in der House-Extension).
