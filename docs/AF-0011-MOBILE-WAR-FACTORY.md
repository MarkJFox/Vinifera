# AF-0011 – Mobile Waffenfabriken für die Advanced AI

Stand: 2026-11-23, Branch `feature/af-0011-ai-mobile-war-factory` (von `aftermath-dev`). Kompiliert (RelWithDebInfo). Im Skirmish getestet: Nod baut die Fabrik, fährt zum Frontpunkt und entfaltet sie dort. Umzug, Speichern/Laden und Multiplayer sind noch nicht getestet.

## Ausgangslage

Eine mobile Waffenfabrik ist ein Fahrzeug, dessen `DeploysInto=`-Gebäude `IsMobileWar=yes` hat (Aftermath: `MOBWARN` → `DNWEAP`, `MOBWARG` → `DGWEAP`).

Bisher hat die KI sie weder gebaut noch entfaltet:

- Die Advanced AI wählt Fahrzeuge nach ihrem Kampfwert für ein Team. Unbewaffnete Fahrzeuge bekommen keinen Wert und werden nie gewählt.
- TS entfaltet KI-Fahrzeuge nur, wenn sie ein MCV sind (`BuildConst`), wenn ein Team-Skript „Deploy“ enthält oder wenn `DeployToFire=yes` gilt und ein Ziel vorhanden ist.
- Scheitert `Mission_Unload` bei der KI, schickt TS das Fahrzeug auf `MISSION_HUNT`. Eine unbewaffnete Fabrik fährt dann auf den Gegner zu.

## Was `IsMobileWar=yes` am Gebäude bewirkt (TS)

- Beim Fertigstellen wird das Gebäude Primärfabrik (laut OpenTS-Nachbau `Toggle_Primary()`). Neue Fahrzeuge kommen dann dort heraus.
- Verkaufen bedeutet Einpacken: `Mission_Deconstruction` (0x4306C8 → 0x443EA0 `IsLimpetMine || IsMobileWar`) macht immer wieder das Fahrzeug daraus, auch bei der KI und ohne `MCVRedeploy`.
- Die Deploy-Taste darf das Gebäude einpacken.
- Beim Aufziehen eines Auswahlrahmens wird es nicht mit ausgewählt.
- Ingenieure können es übernehmen oder reparieren, obwohl es als Fahrzeug zählt.

Die Einstellung gibt es nur bei Gebäudetypen; das Fahrzeug braucht nur `DeploysInto=`.

## Ablauf

**Bauen** (in `AdvAI_AI_Unit`, nach Sammlern, Luftabwehr und festen Taktik-Teams, vor den dynamischen Teams). Alle Bedingungen müssen gelten:

- `AdvancedAIMobileWarFactory=yes` und die Spielzeit liegt über `AdvancedAIMobileWarFactoryDelay`.
- Seit dem letzten Verlust ist `AdvancedAIMobileWarFactoryRebuildDelay` vergangen.
- Keine Bedrohung durch einen frühen Ansturm, und die KI wurde in der letzten Minute nicht angegriffen.
- Die KI hat keine mobile Fabrik, weder als Fahrzeug noch als Gebäude noch in Produktion.
- Die KI hat eine feste Waffenfabrik, also keine mobile und keine Werft.
- Es gibt einen Frontpunkt (siehe unten).
- Der Typ ist für die KI baubar: TechLevel, Besitzer und Voraussetzungen passen.
- Die normale Budgetprüfung der Einheitenproduktion lässt den Start zu, und danach bleiben mindestens `AdvancedAIMobileWarFactoryReserve` Credits übrig.

**Frontpunkt:**

1. Gegner ist das aktuelle Feindhaus, sonst das nächste aktive gegnerische Haus mit Basis.
2. Der Punkt liegt `AdvancedAIMobileWarFactoryFrontRatio` Prozent des Weges von der eigenen zur gegnerischen Basismitte.
3. Ist er nicht sicher, rückt er in 5-%-Schritten zur eigenen Basis zurück, aber nie näher als 8 Zellen an die eigene Basismitte.
4. **Sicher** heißt: Jedes gegnerische Gebäude ist mindestens `AdvancedAIMobileWarFactorySafeDistance` Zellen entfernt, bewaffnete Gebäude zusätzlich mindestens um ihre Waffenreichweite plus 3 Zellen.

**Bauplatz:** Gesucht wird die nächste Zelle am Frontpunkt, höchstens 12 Zellen entfernt. Sie muss:

- vom Fahrzeug aus erreichbar sein (gleiche Bewegungszone),
- einen gültigen Bauplatz bieten, wie ihn `Try_To_Deploy` prüft (Nordwest-Nachbar),
- rundherum eine Zelle ohne Gebäude haben, für die Ausfahrt und damit nichts zugebaut wird,
- selbst sicher sein.

Bei gleichem Abstand gewinnt der kürzere Weg.

**Fahren und Entfalten:**

- Das Fahrzeug fährt hin und entfaltet sich dort.
- Vorher wird die Platzierung geprüft. Bei einem Fehlschlag bleibt es stehen, statt auf `MISSION_HUNT` zu gehen, und versucht es erneut. Nach drei Fehlschlägen sucht es einen neuen Platz.
- Steht es auf `MISSION_HUNT`, etwa durch einen fremden Unload-Befehl, wird es auf Guard zurückgesetzt.
- Mitglieder von Skript- und Trigger-Teams werden nicht angefasst.

**Umziehen** (nur entfaltet, frühestens `AdvancedAIMobileWarFactoryRelocateDelay` Frames nach dem Entfalten). Die Fabrik packt ein und fährt zum neuen Platz, wenn:

- ihr Platz unsicher geworden ist, oder
- sie mehr als `AdvancedAIMobileWarFactoryRelocateDistance` Zellen vom aktuellen Frontpunkt entfernt steht und der neue Platz mindestens 5 Zellen näher liegt.

Sie zieht nicht um, wenn sie gerade produziert, ein Fahrzeug ausfährt oder die KI kürzlich angegriffen wurde. Sie zieht auch nicht um, wenn sie die einzige Waffenfabrik ist.

Das Ziel wird als Archivziel gesetzt. So macht TS daraus den Fahrbefehl des Fahrzeugs, und das Einpacken setzt keine Überlebenden frei.

**Notfall:** Hat die KI keine feste Waffenfabrik mehr, entfaltet sich das Fahrzeug dort, wo es steht. Der Fahrzeug-Ansturm ohne Bauhof und Fabrik schickt die mobile Fabrik nicht mehr auf Hunt.

**Bauplanung:** Eine entfaltete mobile Fabrik zählt in der Infrastrukturprüfung der Bauplanung (B1) nicht als Waffenfabrik. Geht die feste Fabrik verloren, baut die KI also wieder eine.

## INI

```ini
[AI]
AdvancedAIMobileWarFactory=yes                 ; KI baut und nutzt mobile Waffenfabriken
AdvancedAIMobileWarFactoryDelay=5400           ; Frames bis zur ersten (Standard 6 Minuten)
AdvancedAIMobileWarFactoryRebuildDelay=2700    ; Frames nach einem Verlust (Standard 3 Minuten)
AdvancedAIMobileWarFactoryReserve=1000         ; Credits, die nach dem Kauf übrig bleiben müssen
AdvancedAIMobileWarFactoryFrontRatio=40        ; Prozent des Weges zur gegnerischen Basis, 10-80
AdvancedAIMobileWarFactorySafeDistance=12      ; Zellen Abstand zu gegnerischen Gebäuden, 0-40
AdvancedAIMobileWarFactoryRelocateDistance=20  ; Zellen Abweichung vom Frontpunkt bis zum Umzug, 0 = nie
AdvancedAIMobileWarFactoryRelocateDelay=4500   ; Frames entfaltet, bevor sie umziehen darf (Standard 5 Minuten)
```

Die Standardwerte im Code sind `TICKS_PER_MINUTE`-Vielfache. Die Zahlen oben gelten für 15 Frames pro Sekunde.

Hinweise für die Aftermath-INI:

- `MOBWARG` hat in rules.ini `TechLevel=-1`, und firestrm.ini setzt `TechLevel=-1;10`, also ebenfalls -1. GDI kann die Fabrik deshalb nicht bauen, auch die KI nicht. `MOBWARN` hat in firestrm.ini `TechLevel=10`.
- `MOBWARN` braucht `NAWEAP,NATMPL`.

## Bekannte Grenzen

- Als Primärfabrik gibt die entfaltete Fabrik **alle** neuen Fahrzeuge der KI aus, auch Sammler und MCVs. Sie fahren dann von der Front zurück.
- Pro Haus wird nur eine mobile Fabrik gesteuert (`BuildLimit=1`).
- Der Frontpunkt richtet sich nach den Basismitten, nicht nach den Zielen der laufenden Taktik.
- Gefahr wird nur an gegnerischen Gebäuden gemessen, nicht an Einheiten.

## Diagnose

Im Entwicklermodus schreibt das Debug-Log Zeilen mit `AF-0011:`:

| Ereignis | Bedeutung |
|---|---|
| `mobile-war ordered` | Bauauftrag erteilt |
| `mobile-war ... site` | Bauplatz gewählt, mit Frontpunkt, ggf. `(emergency)` |
| `mobile-war no site near` | Kein Platz gefunden, neuer Versuch nach 10 Sekunden |
| `mobile-war deploy` | Entfalten begonnen |
| `mobile-war site ... failed` | Drei Fehlversuche, neue Suche |
| `mobile-war relocate` | Einpacken und Umzug, Grund `front moved` oder `unsafe` |
| `mobile-war lost` | Keine mobile Fabrik mehr, Wiederaufbau-Sperre beginnt |
| `mobile-war skip reason=…` | Warum kein Bauauftrag kommt; bei Änderung, höchstens einmal pro Minute je Haus (siehe unten) |

Gründe bei `skip`: `delay`, `rebuild-delay`, `start-rush-threat`, `recently-attacked`, `no-fixed-war-factory`, `no-front-point` (mit Gegner-Haus), `cannot-produce` (TechLevel, Besitzer oder Voraussetzungen), `budget` (mit Grund der Budgetprüfung, z. B. `reserved-funds`, `economy-phase`), `money` (zu wenig übrig nach `AdvancedAIMobileWarFactoryReserve`), `no-mobile-war-factory-type`.

**Team-Budget:** Die Budgetprüfung der Einheitenproduktion behandelt jedes Fahrzeug als Auftrag für das Team, das gerade produziert wird, und sperrt es mit `team-budget`, wenn es dessen Kostengrenze sprengt. Die mobile Fabrik gehört zu keinem Team und ist wie Sammler und MCVs davon ausgenommen; sie zählt auch nicht gegen das Budget anderer Aufträge. Ohne diese Ausnahme kam im ersten Skirmish-Test kein Bauauftrag zustande.

## Offene Tests

- Skirmish Nod gegen eine KI: Bau nach etwa 6 Minuten, Fahrt, Entfalten am Frontpunkt, neue Fahrzeuge kommen dort heraus.
- Gegner baut Verteidigung in Reichweite: Die Fabrik zieht nach der Sperrzeit zurück.
- Feste Waffenfabrik zerstören: Das Fahrzeug entfaltet sich sofort; die Bauplanung baut die feste Fabrik neu.
- Mobile Fabrik zerstören: Neubau erst nach der Wiederaufbau-Sperre.
- Speichern und Laden während Fahrt, Entfalten und Umzug.
- Multiplayer-Sync mit zwei KI-Häusern.

## Dateien

| Datei | Inhalt |
|---|---|
| `src/extensions/house/houseext_advai_mobilewar.cpp` | Bauen, Frontpunkt, Platzsuche, Fahren, Entfalten, Umziehen |
| `src/extensions/house/houseext.h/.cpp` | Zustandsfelder (werden mitgespeichert), CRC |
| `src/extensions/house/houseext_advai_units.cpp` | Bauauftrag in `AdvAI_AI_Unit`, Aufruf in `AdvAI_Expert_AI`, Ausnahme beim Fahrzeug-Ansturm |
| `src/extensions/house/houseext_advancedai.cpp` | B1-Infrastruktur zählt mobile Fabriken nicht als Waffenfabrik |
| `src/extensions/rules/rulesext.h/.cpp` | INI-Schlüssel |

Speicherstände aus älteren Builds sind nicht kompatibel (neue Felder in der House-Extension).
