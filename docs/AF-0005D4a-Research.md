# AF-0005D4a - CustomFoundation Ground Service / Repair Bay research

Stand: 2026-11-01. Arbeitsrepo: `C:\dev\Aftermath-Engine\Aftermath-Vinifera`,
Branch `aftermath-dev`, HEAD `374cd9148fb01268aa7317c331859febf24e689a`
(`feat: support fixed-wing aircraft multidock`). Der Working Tree war zu Beginn
sauber. Aktuelle Save-Basis: **0x190000**, in diesem Auftrag unverändert.

**Ergebnis: GO WITH RESTRICTIONS, Umfang MEDIUM für einen einzelnen, eng
abgegrenzten Ground-Servicepunkt. Kein Coordinate-Override-only-Fix.**
TS besitzt bereits die benötigte Einzelreservierung und eine ausdrückliche
Durchfahrtsausnahme für Repair-Buildings. Es braucht deshalb voraussichtlich
keine Ground-Visitor-Array-, Queue- oder Locomotor-Neuarchitektur. Es braucht aber
mehrere konsistente Koordinatenadapter, einen passenden Ausgang und die Behandlung
der koordinatenabhängigen Verkaufs-, Capture- und AI-Pfade. Das ist erheblich mehr
als ein zusätzlicher Docking_Coord-Return. Beliebige Locomotoren, isolierte Masken,
Halo-Service und parallele Ground-Slots wären ein deutlich größerer Auftrag.

Dies ist ausschließlich statische Research. Keine Implementation, Sourceänderung,
Hookinstallation, Builds, Spielstarts oder Tests. Alle folgenden D4b-Bausteine und
INI-Namen sind Vorschläge, keine vorhandenen Features oder beschlossene API.

## 1. Belege und Aussagegrenzen

Primär für den tatsächlichen TS-Vertrag: das lokale Original-PE
`C:\dev\Aftermath-Engine\references\ts-patches\input.dat`, mit den aktuellen
TS++-Deklarationen und Vinifera-Hooks abgeglichen. Einzelne Funktionen/Spannen
wurden mit `dumpbin /disasm:bytes /range:...` gelesen; die Datei wurde nicht
ausgeführt. Auch die beiden RA2/YR-PEs wurden nur gelesen. Kein Recomp/Lift/Build.

| Binärdatei | SHA256 |
|---|---|
| TS input.dat | BC4C15D826AAEBEACE3FEC6360EB687E5662CBA8745605093254931DCDB3AE1B |
| files/game.exe | 48C34DF90FBCE54916291700A39D1BBEFD0B4042F0715C6A5D27D0E7596EB0BD |
| files/gamemd.exe | 7B8A068535D6AF06845EDF95AE829B113D00C02909330E16F197426CD7DB94B6 |

Lesbarer Vergleichscode stammt aus OpenTS sowie den veröffentlichten TD-/RA- und
Remastered-Quellen. **OpenTS ist ein weiterentwickelter Port, kein wortgetreues
Decompilat des untersuchten TS-PE.** Seine Namen und Abläufe helfen beim Lesen;
TS-Adressen, Instruktionen und kritische Bedingungen wurden am PE geprüft.
Beispielsweise darf OpenTS' modernisierte Serialisierung oder Distanzarithmetik
nicht als shipping TS-Implementation ausgegeben werden.

Quellenindex, alle Pfade lokal und read-only außer dieser Research-Datei:

| Quelle | Relevante Stellen |
|---|---|
| [TS++ definitions](C:/dev/Aftermath-Engine/Aftermath-Vinifera/TSpp/src/tspp/tspp_definitions.cpp) | Native Funktionsadressen; Unit/Foot/Building/Radio und Repair_Cost/Step |
| [Building hooks](C:/dev/Aftermath-Engine/Aftermath-Vinifera/src/extensions/building/buildingext_hooks.cpp) | RX ab 2429, Service-Abfahrt 1460, vorhandene D1/D3-Hooks |
| [BuildingType extension](C:/dev/Aftermath-Engine/Aftermath-Vinifera/src/extensions/buildingtype/buildingtypeext.cpp) | Scope-Validator, Koordinatenvorbild, raw Save/Load und CRC |
| [OpenTS unit.cpp](C:/dev/Aftermath-Engine/references/OpenTS/code/unit.cpp) | Per_Cell_Process, Can_Enter_Cell, Assign_Destination, Mission_Repair |
| [OpenTS foot.cpp](C:/dev/Aftermath-Engine/references/OpenTS/code/foot.cpp) | Mission_Enter, RX, Can_Demolish, Detach |
| [OpenTS building.cpp](C:/dev/Aftermath-Engine/references/OpenTS/code/building.cpp) | RX, Guard, Repair, Capture, Deconstruction, Find_Exit_Cell |
| [OpenTS drive.cpp](C:/dev/Aftermath-Engine/references/OpenTS/code/drive.cpp) | Process/While_Moving: Zielaufnahme und Ankunft |
| [OpenTS radio.cpp](C:/dev/Aftermath-Engine/references/OpenTS/code/radio.cpp) | Skalarer Kontakt, HELLO/OVER_OUT, Limbo/Detach |
| [OpenTS techno.cpp](C:/dev/Aftermath-Engine/references/OpenTS/code/techno.cpp) | Tether, Reparaturschritt, Find_Docking_Bay, Capture |
| [RA building](C:/dev/Aftermath-Engine/references/CnC_Red_Alert/CODE/BUILDING.CPP), [TD building](C:/dev/Aftermath-Engine/references/CnC_Tiberian_Dawn/BUILDING.CPP) | Frühere Repair-State-Machines |
| [Remastered RA](C:/dev/Aftermath-Engine/references/CnC_Remastered_Collection/REDALERT/BUILDING.CPP), [Remastered TD](C:/dev/Aftermath-Engine/references/CnC_Remastered_Collection/TIBERIANDAWN/BUILDING.CPP) | Vergleich der veröffentlichten Varianten |
| [YRpp RadioClass](C:/dev/Aftermath-Engine/references/Phobos/YRpp/RadioClass.h), [BuildingType](C:/dev/Aftermath-Engine/references/Phobos/YRpp/BuildingTypeClass.h) | RadioLinks, NumberOfDocks, DockingOffsets |
| [YRpp ObjectClass](C:/dev/Aftermath-Engine/references/Phobos/YRpp/ObjectClass.h) | GetDockCoords mit Docker-Argument |
| [Phobos Building hooks](C:/dev/Aftermath-Engine/references/Phobos/src/Ext/Building/Hooks.cpp) | UnitRepair ab 572, Repairrate/-step/-cost |
| [Ares Building hooks](C:/dev/Aftermath-Engine/references/Ares/src/Ext/Building/Hooks.cpp), [Antares Building hooks](C:/dev/Aftermath-Engine/references/Antares/src/Ext/Building/Hooks.cpp) | Unabhängige UnitReload-Docker, keine Ground-Multi-Service-Vorlage |
| [Antares Sell hooks](C:/dev/Aftermath-Engine/references/Antares/src/Ext/Building/Hooks.Sell.cpp) | CanBeSold benutzt tatsächliche Dock-Koordinate |
| [DTA Building hooks](C:/dev/Aftermath-Engine/references/DTA-Vinifera/src/extensions/building/buildingext_hooks.cpp) | Vergleich des bestehenden Vinifera-RX-/Rally-Vertrags |
| [Depot-Stop-Patch](C:/dev/Aftermath-Engine/references/ts-patches/src/fix_depot_explosion_glitch.asm) | Bestehendes natives Abort-/Occupation-Risiko |
| [redalert2-recomp](C:/dev/Aftermath-Engine/references/redalert2-recomp/README.md) | Lokaler Checkout enthält Runtime/Tooling, keinen gelifteten Engine-C-Ordner |

## 2. Native Erkennung und Eintritt

`UnitRepair=yes` wird als BuildingTypeClass::IsCanUnitRepair gelesen. Es ist kein
Ground-`Dock=`-Slot-System. Die manuelle Interaktion fragt mit RADIO_CAN_LOAD an;
die Repair Bay akzeptiert grundsätzlich geeignete Units und auch Aircraft,
abhängig von Allianz, Bauzustand und IsOn. Ein UnitRepair-Gebäude ist deshalb nicht
automatisch ein rein bodenfahrzeugspezifischer Typ.

Der eigentliche Weg wird über **UnitClass::Assign_Destination 0x00657520**
aufgebaut: ein Building-Objekt als Ziel, Kontaktaufnahme über HELLO/DOCKING und
anschließend der normale Foot-/Drive-Zielpfad. Die Eintrittsmission ist
**FootClass::Mission_Enter 0x004A4820**. Eine bereits gequeue-te Enter-Mission wird
ebenfalls berücksichtigt; normale Move-Aufträge auf eine Repair Bay besitzen
zusätzlich die besondere Repair-Building-Kontaktbehandlung. Die menschliche
Cursor-/Action-Entscheidung ist von diesem tatsächlichen Eintrittspfad zu trennen.

Für einen bestehenden Repair-Auftrag sucht **UnitClass::Mission_Repair
0x00656D20** über Find_Docking_Bay nach `Rule->RepairBay`, versucht HELLO und
weist bei Erfolg MISSION_ENTER zu. MISSION_REPAIR auf der Unit ist also die
Such-/Zuführungsmission, nicht der Code, der ihre Strength kontinuierlich erhöht.

| Funktion | Adresse | Rolle |
|---|---|---|
| Unit::Assign_Destination | 0x00657520 | Building-Identität, Handshake, Busy-/Archive-Verhalten |
| Foot::Assign_Destination | 0x004A49F0 | NavCom setzen, Destination_Coord abfragen, Locomotion::Move_To |
| Foot::Mission_Enter | 0x004A4820 | Kontakt/ArchiveTarget wählen, DOCKING wiederholen, Abbruch |
| Unit::Per_Cell_Process | 0x006511B0 | Tatsächliche Ankunft, IM_IN, Locomotor Power_Off |
| Building::Mission_Guard | 0x004300D0 | Zusätzliche Erkennung eines angekommenen Enter-Kunden |
| Building::Receive_Message | 0x004268C0 | Nativ; aktuell durch Vinifera-C++ ersetzt |
| Building::Mission_Repair | 0x00431880 | INITIAL/IDLE/DURING für UnitRepair, weitere Rollen in anderen Zweigen |
| Techno::Receive_Message | 0x0062AC40 | Tether und eigentlicher RADIO_REPAIR-Schritt |
| Foot::Receive_Message | 0x004A4560 | ON_DEPOT, NEED_TO_MOVE, bewegungsbedingte Reparaturablehnung |
| Unit::Receive_Message | 0x0064F440 | Unit-spezifische Transport-/Docking-Nachrichten |

## 3. Radio, Tether, Reservation und mehrere Fahrzeuge

Beide Seiten besitzen einen normalen **RadioClass::Radio**, Offset **+78h**.
Erfolgreiches HELLO stellt die wechselseitige Beziehung her: Unit.Radio=Building,
Building.Radio=Unit. Radio::Receive_Message liegt bei **0x005BD880**, TX mit
param bei **0x005BD940**, TX ohne explizites param bei **0x005BDA10**.
HELLO an einen anderen Partner beendet zunächst den alten Kontakt. HELLO-RX kann
einen fremden bestehenden Kontakt ablehnen; wiederholtes HELLO desselben Partners
ist zulässig. Diese skalare Beziehung ist die Reservation vor Erreichen des Pads.

| Nachricht | Bedeutung im Ground-Vertrag |
|---|---|
| CAN_LOAD | Eignungs-/Zustandsfrage, keine Reservation und kein Slot-Insert |
| HELLO | Eigentliche gegenseitige Einzelreservierung; der erste erfolgreiche Kontakt gewinnt |
| DOCKING | Wiederholte Wartung durch Mission_Enter/Assign_Destination; Building prüft Zustand und ggf. NEED_REPAIR |
| NEED_REPAIR | Kann weitere Arbeit am Depot sinnvoll sein? Nicht die physische Ankunft |
| NEED_TO_MOVE | ROGER bei fehlendem NavCom oder nicht bewegendem Locomotor; param kann NavCom liefern |
| MOVE_HERE | Generischer Foot-Bewegungsauftrag; seine Zellvergleichslogik benutzt das übergebene Objektzentrum. Der normale UnitRepair-Zweig sendet ihn nicht wie der Helipad-/Refinery-Zweig als zentralen Ground-Anfahrtsbefehl |
| ON_DEPOT | Foot prüft `Map[Center_Coord()].Cell_Building() == from`; weder explizite ServiceCell noch bloß Radio-Kontakt |
| IM_IN | Physische Eintrittsmeldung; Building queuet REPAIR, Unit SLEEP, setzt IsReadyToCommence |
| TETHER / UNTETHER | Techno setzt/löscht IsTethered (+1FDh) und bestätigt rekursiv beim Partner |
| REPAIR | Foot lehnt bei NavCom!=null ab; sonst Techno bearbeitet Gesundheit, Kosten und Spezialservice |
| ALL_DONE / CANT / NEGATIVE | Fertig / nicht bezahlbar / kein gültiger weiterer Reparaturschritt |
| OVER_OUT | Sender leert seinen skalaren Partner, Empfänger beendet den passenden Kontakt; Techno entfernt vorhandenen Tether |
| RUN_AWAY / UNLOADED | Abbruch/Scatter bzw. Räumungsbehandlung; keine neue Queue |

**Tether ist keine zwingende Eintrittsberechtigung für die Repair Bay.**
Der normale Ground-Pfad kann mit HELLO/DOCKING -> IM_IN ohne ein vorausgehendes
TETHER reparieren. Die belegte-Zellen-Ausnahme erlaubt ausdrücklich auch den
Repair-Building-Radiopartner ohne Tether. Die Repair-Startbedingungen testen nicht
pauschal IsTethered. Ein künstliches obligatorisches HELLO->TETHER-Protokoll wäre
eine Semantikänderung. Vorhandene Tether werden beim allgemeinen Release entfernt.

Nativ gibt es keinen Ground-Visitor-Array, keine First-Free-Auswahl und keine FIFO.
Nur ein Partner kann reserviert sein. Weitere manuell geschickte Units behalten
teilweise Building in ArchiveTarget und setzen `field_20C` als Nearby-/Wartebezug,
erhalten noch keinen eigenen Servicekontakt und versuchen den Auftrag erneut.
Das konkrete räumliche Warten hängt von Mission, vorhandener Navigation, Umgebung
und Blockade ab; eine geordnete Umkreisung oder Reihenfolge ist nicht garantiert.
Bei DOCKING-Ablehnung ohne Tether kann Foot::Mission_Enter OVER_OUT senden und in
Idle wechseln. Ein MISSION_REPAIR-Sucher kann warten und später erneut suchen.

**Vinifera-Unterschied:** Im ersetzten Building-RX liegt die allgemeine
CAN_LOAD-Busy-Sperre inzwischen hinter den besonderen Repair-/Helipad-/Hospital-
Antworten. Eine positive Repair-CAN_LOAD-Antwort ist daher ausdrücklich kein
Beweis für freie Kapazität. HELLO und der tatsächliche skalare Kontakt verhindern
weiterhin die doppelte Reservation. Find_Docking_Bay darf nicht als neue Queue
interpretiert werden. Bei einem ohne only-unoccupied-Filter gewählten besetzten
Depot kann der folgende HELLO scheitern und der Sucher erneut warten.

## 4. Die heutigen Koordinaten sind mehrere Pfade

**Der normale TS-Ground-Servicepunkt ist Building::Center_Coord, nicht ein
separates Ground-Dock-Offset.** Native Center_Coord 0x0042F250 berechnet:

```text
Position/Get_Coord + (128*(Width-1), 128*(Height(false)-1), 0)
```

CELL_LEPTON=256. Das ist ein Lepton-Punkt, bei geraden Dimensionen nicht zwingend
der Mittelpunkt einer einzelnen Zelle. TS++ nennt den virtuellen
Destination_Coord-Slot **entry_50**. Building::entry_50 **0x0042F3B0** ruft bei
Helipad Docking_Coord (+A8h), sonst Center_Coord (+4Ch) auf. UnitRepair allein
besitzt dort keinen Sonderfall. Docking_Coord **0x0042F2C0** fällt für eine reine
Repair Bay ebenfalls auf Center_Coord zurück; Weeder/Refinery haben andere Pfade.
BuildingShape/BibShape liefern keine Ground-Serviceposition.

| Verbraucher | Tatsächlicher Pfad / Stelle | D4b-Folge |
|---|---|---|
| Erste Anfahrt | Foot::Assign_Destination, NavCom->entry_50 bei 0x004A4AD6, Ergebnis ab 0x004A4AD9 | Eigenen Punkt liefern, Building als NavCom behalten |
| Drive-Neuzielaufnahme | Drive::Process 0x0047DDA0, Zielcall 0x0047E1B4 | Derselbe Punkt; sonst zieht der Driver wieder zum Zentrum |
| Drive-Ankunftszelle | Drive::While_Moving 0x0047E740, NavCom-Zielcall 0x0047F854 | Ebenfalls Punkt-Zelle; nur die erste Zielaufnahme zu ändern reicht nicht |
| Driver-Z-Höhentest | 0x0047F8A7 ruft entry_50 der Unit selbst auf | Keine Building-Koordinate substituieren |
| Unit-Ankunft | Per_Cell_Process 0x00651333..0x006514AC; tatsächliche Building-Zellkoordinate aus Call 0x0065135D | Vergleicht Unit- und Building-Center-Zelle, Building-RTTI und NavCom==Kontakt |
| Guard-Serviceübergang | Building::Mission_Guard, Center-Calls 0x004302D6 (Besucher), 0x004302E4 (Building) | Distanz zum Servicepunkt statt Gebäudezentrum |
| Repair INITIAL | 0x0043243A Besucher-Center, 0x00432449 Building-Center | Native Näheprüfung zum selben Punkt |
| Repair IDLE / Locomotor | 0x00431F1E Besucher-Center, 0x00431F2D Building-Center, `<150` | Abschalten/NavCom-Räumen nicht am alten Zentrum auslösen |
| DOCKING maintenance | Aktuelles Building-RX, IsCanUnitRepair-Distance_To-Check um Zeile 2638 | Distanz zum Servicepunkt bei Ground-Kontakt |
| ON_DEPOT | Foot RX: tatsächliche Map-Zelle -> Cell_Building | Bei occupied Servicezelle bereits passende Semantik |
| Unit-Verkauf | Foot::Can_Demolish 0x004A5230, Partner-Center 0x004A5280, Distanz `<128` | Gleichen Punkt berücksichtigen |
| Depot-Verkauf | Building::Mission_Deconstruction 0x00430630, Unit-Center 0x00431019, Building-Center 0x0043102A | Sonst kann die Entscheidung Unit verkaufen / Gebäude verkaufen falsch sein |
| Capture mit Kunde | Building::Captured 0x0042F660, Docking_Coord-Call 0x0042F827, Besucher-Center 0x0042F838 | Naher Kunde kann mitcaptured werden, ferner wird weggeschickt: Servicepunkt nötig |
| AI-/Missionssuche | Find_Docking_Bay 0x00637E60: Building-Center 0x00637F37, eigene entry_50 0x00637F74, Zonenprüfung 0x00637FD7 | Kandidatenzone muss zur tatsächlichen Servicezelle passen |
| Kandidatenranking | 0x00637FF5 Object::Distance_To 0x00586CC0 zum Building | Derzeit Zentrum; bewusste Ranking-Policy nötig, keine globale Center-Änderung |
| Fertig-Abfahrt | Find_Exit_Cell 0x00434140, Native ExitList bzw. Rechtecksuche; Vinifera Rally-Helper | Eigenständiger Ausgang, kein Servicepunkt und kein generisches Exit_Coord |

Diese Liste umfasst die servicebezogenen direkten und indirekten Center-/Dock-
Verbraucher im untersuchten Ground-Pfad. Allgemeine Center-Aufrufe für Darstellung,
Waffen, Sicht, Bedrohung und Team-Regroup sind keine Serviceposition und bleiben
unverändert. Kein globaler Center_Coord-Ersatz.

## 5. Physische Anfahrt und Building-Occupation

NavCom bleibt das **Building-Objekt**, nicht die Map-Cell des gewünschten Punktes.
Foot übersetzt dessen Destination_Coord in eine Coord für ILocomotion::Move_To.
Der Driver nutzt eine zellbasierte Route, Track-/Facing-/Occupation-Prüfungen und
eine finale Lepton-Zielkoordinate. Er fragt die Destination nochmals während der
Bewegung ab. Unit::Per_Cell_Process(PCP_END) meldet den tatsächlichen Eintritt.

**UnitClass::Can_Enter_Cell 0x00655330 besitzt zwei relevante native Ausnahmen:**

1. 0x0065575C..0x00655789: Ist ein Occupier der eigene Radio-Partner und ist
   IsTethered gesetzt **oder** dieser Partner ein IsCanUnitRepair-Building, geht
   der Pfad zum MOVE_OK-Return. Die tatsächliche Building-Occupation wird nicht
   entfernt. Das ist eine lokale Erlaubnis innerhalb der normalen Zellprüfung.
2. 0x0065583E..0x006558A2: Beim Weiterfahren über eine Repair Bay kann deren
   Building-Occupier übersprungen werden, wenn die Zelle hinter der Bewegungsrichtung
   zum selben Building gehört. Das unterstützt das Weiter-/Herausfahren auf
   mehrzelligen Depots, auch nach dem Kontaktende.

Vorherige Prüfungen für Terrain, Erreichbarkeit, Höhe/Brücke, Bewegungstyp usw.
verschwinden dadurch nicht. Die frühe MOVE_OK-Antwort ist außerdem keine neue
Kollisionsvermeidung für mehrere Fahrzeuge auf einer großen Maske. Man darf sie
nicht zu einer globalen passierbaren Foundation für beliebige Units erweitern.

Für ein flaches, zusammenhängendes, erreichbar angebundenes CustomFoundation-Pad
ist deshalb **keine neue pauschale Passability-Ausnahme belegt nötig**. Das
Building ist der autorisierende Occupier. Ein Fahrzeug darf auf dessen belegte
Servicezelle fahren. Bei großen/disjunkten Masken, schmalen Durchgängen, fremden
Occupiern, Brücken und ungeeigneten MovementZones bleiben echte Pfadprobleme.
Width/Height oder ein gültiges X/Y garantieren keine erreichbare Fahrstrecke.

Würde man NavCom durch eine freie Cell ersetzen, verlöre man dagegen die
Building-Identität, auf die Unit-Ankunft und Radio-Koordination angewiesen sind.
Dies ist keine sichere Abkürzung für D4b.

## 6. Reparaturstart, Rate, Kosten und Completion

Beim PCP_END prüft die Unit Enter/Patrol, vorhandenen Building-Kontakt,
Gleichheit der Center-Zellen und NavCom==Kontakt. Der native Hover-Sonderfall
kann NavCom ergänzen. Bei Ankunft: Foot::Per_Cell_Process, RADIO_IM_IN an den
Partner, anschließend Locomotion::Power_Off. Building-IM_IN setzt
IsReadyToCommence, queuet MISSION_REPAIR und beim Besucher MISSION_SLEEP.
Building::Mission_Guard besitzt zusätzlich einen Weg aus Guard nach Repair,
wenn ein Enter-Kunde nah und nicht mehr weiterfahrend ist.

Die eigentliche Building-Reparatur startet nicht allein durch CAN_LOAD oder
Nähe eines beliebigen beschädigten Tanks:

| Zustand | Bedingungen und Effekt |
|---|---|
| INITIAL=0 | Radio-Partner erforderlich, sonst Guard/Animation aufräumen. NEED_TO_MOVE==ROGER und Distanz zum Zentrum `<64` Leptons; für Hover-Locomotion native Toleranz 150. Dann IDLE, Delay 3 Ticks. Sonst ggf. Locomotor Power_On, außer IonStorm |
| IDLE=1 | Bei Distanz `<150`: noch bewegenden Locomotor abwarten, stehenden und noch eingeschalteten Locomotor ausschalten; ist er abgeschaltet, NavCom löschen. NEED_TO_MOVE muss Reparatur erlauben |
| Erster Schritt | IDLE kann bereits RADIO_REPAIR senden, bevor der DURING-Timerzyklus läuft; nicht jeder Schritt beginnt erst nach einer vollständigen Rateperiode |
| DURING=2 | Building-Stage/Timer fortschreiben; Stage >= Rule.URepairRate*900 und NEED_TO_MOVE==ROGER -> Stage zurücksetzen, RADIO_REPAIR |
| Antwort ROGER | Weiter reparieren |
| CANT | Kein bezahlbarer Schritt: Meldung/Animation zurück zu IDLE, nicht als neue erfolgreiche Completion behandeln |
| ALL_DONE bzw. anderer Abschluss | Animations-/Statusabschluss und vorhandene Abfahrtslogik; bereits unbeschädigte Besucher haben einen eigenen Abfahrtszweig |

Power_On dient dem erneuten Anfahren/Abfahren, nicht dem aktiven Reparaturschritt.
Nähe allein ersetzt weder Partner noch Navigationszustand.

**Strength erhöht Techno::Receive_Message(RADIO_REPAIR), 0x0062AC40.** Foot
verweigert diesen Schritt bei NavCom!=null. Techno setzt zuvor native
Limpet-/Facing-Servicezustände zurück; ManualReload ist ein zusätzlicher nativer
Sonderfall. Bei HealthRatio < Rule.ConditionGreen fragt es den Typ nach
Repair_Cost und Repair_Step (effektiv mindestens 1 Health), prüft das Geld des Kunden-Hauses, zieht die Kosten ab,
erhöht Strength und liefert ROGER oder ALL_DONE. Bei Erreichen des Abschlusses
wird auf MaxStrength begrenzt. Ohne Geld: CANT, ohne weiteren Bedarf: NEGATIVE.

Repair_Cost **0x0063B920** verwendet Raw_Cost, MaxStrength, Rule.RepairStep und
Rule.RepairPercent, mit den nativen Integer-Zwischenschritten und Minimum 1.
UnitType::Repair_Step **0x0065C420** und der Basiswert **0x0063B970** lesen
Rule.RepairStep. TS verwendet hier nicht eine neue D3c1-Reloadrate oder einen
selbst erfundenen UnitRepairStep-Key. URepairRate ist vom normalen
Building-Selbstreparaturwert RepairRate und vom Aircraft-ReloadRate zu trennen.
Kosten, Health-Schritte, Stage und Antwortsemantik sollen vollständig nativ bleiben.

## 7. Abfahrt und Konkurrenz nach Service

Bei erfolgreichem Ende erhält die Unit normalerweise MISSION_MOVE mit einer
Exit-Cell. Für AI kann ein archiviertes Ziel wiederaufgenommen werden. Die
vorhandenen Vinifera-Hooks verwenden stattdessen bevorzugt Building.ArchiveTarget
als Rally-Ziel, sonst Find_Exit_Cell. Nach Zuweisung werden RADIO_OVER_OUT und
das Räumen von `field_20C` ausgeführt. Native Unit::Assign_Destination besitzt
eine Power_On-Behandlung für eine ausgeschaltete Unit auf einem Repair-Building;
deshalb ist die tatsächliche Cell-Occupation auch bei der Abfahrt relevant.

**Exit_Coord 0x00434580 ist nicht der normale Repair-Abfahrtsvertrag.**
Find_Exit_Cell **0x00434140** verwendet zuerst native Sonderpräferenzen/ExitList;
bei fehlender Liste durchsucht es deterministisch Rechteckränder und ruft
Can_Enter_Cell des Besuchers auf. Es ist keine pauschale zufällige Nearby-Zelle.
Nearby_Location/Scatter werden zusätzlich bei Warten, Run-Away und Räumung genutzt.

Wichtig für große CustomFoundations: BuildingType::ExitList wird aus
**ExitLists[Size]** der nativen Fallback-Foundation gesetzt, auch beim Laden.
Die bestehenden Custom-Width/Height-Hooks ersetzen diese Liste nicht. D1s
Find_Exit_Cell-Adapter gilt bisher nur für passende Infantry-Factories. Eine
Repair Bay mit größerer Maske braucht einen lokal begrenzten Ground-Exit-Adapter,
sonst kann eine vermeintliche Exit-Cell noch mitten im belegten Custom-Building
liegen. Der Perimeter muss zur tatsächlichen Maske/BBox gehören und fahrbar sein.

Wenn keine Exit-Cell vorhanden ist, kann die vorhandene Logik den Kunden bzw.
Kontakt behalten und später erneut versuchen. „Nach Repair immer sofort frei“
ist nicht garantiert. Ein neuer Move-Auftrag kann Service abbrechen, den
Locomotor wieder aktivieren und über den nativen Release-/Räumungspfad abfahren;
er darf nicht vom Adapter in einen neuen Repair-Auftrag umgewandelt werden.

Nach OVER_OUT ist die einzelne Reservation frei. Ein wartender zweiter/dritter
Tank kann dann beim nächsten erfolgreichen HELLO übernehmen. Das garantiert
weder FIFO noch einen kollisionsfreien Wechsel an der Gebäudekante.

## 8. Lifecycle und besondere Risiken

| Ereignis | Bestehender Pfad / Aussage für D4 |
|---|---|
| Unit zerstört / gelöscht | Foot::Detach_All 0x004A4BC0 sendet OVER_OUT; globale Detach-Benachrichtigungen räumen native Referenzen. Radio::Detach 0x005BDA30 leert den passenden Partner bei all=true. Kein eigener Ground-Visitor-Zeiger nötig |
| Building zerstört | Building::Detach_All 0x00434000, derzeit Vinifera-Ersatz, sendet bei all OVER_OUT; allgemeine Techno-/Object-Detach- und Limbo-Pfade folgen. Kontaktlose Repair-Mission fällt nach Guard zurück |
| Unit oder Building limbo | Radio::Limbo 0x005BD9E0 sendet OVER_OUT vor Basis-Limbo; Unit::Limbo 0x00659270 und Building::Limbo 0x0042DB50 besitzen weitere eigene Cleanup-Arbeit |
| Sell / Deconstruction | Building::Mission_Deconstruction kann den nahen Kunden statt des Gebäudes verkaufen. Center-Test und Foot::Can_Demolish müssen zum neuen Punkt passen; erst spätere Limbo-/Detach-Schritte beenden einen tatsächlichen Gebäudeabbau |
| Owner change | Building::Captured prüft den stehenden nahen Kunden mit Docking_Coord/Distanz; kann ihn mitcapturen oder RUN_AWAY/OVER_OUT senden. Basis-Captured/Detach_All(false) räumt unpassende Allianzkontakte. Kein blindes „Capture räumt immer alles“ |
| EMP / Power loss | Kein automatisches Löschen der Reservation. RX weist neue CAN_LOAD/DOCKING bei !IsOn ab. Die untersuchte UnitRepair-State-Machine besitzt kein mit D3c1-Aircraft_Service identisches Is_Powered_On-Gate pro Tick. IsOn, StunDuration, Hausenergie und Locomotor-Power sind unterschiedliche Zustände; keine zusätzliche Pause-/Release-Policy für D4 erfinden |
| IonStorm | Native Power_On-/Locomotor-Ausnahmen erhalten. D4 ist kein Ersatz für IonStorm-/Hover-Verhalten |
| Unit teleportiert / detached | Reguläres Limbo/Detach/Assign_Destination benutzt vorhandene Cleanup-Pfade. Ein bloßes Set_Coord ist kein bewiesener Service-Abbruch; Script-/Teleport-Sonderpfade müssen später geprüft werden |
| Mission abort / neuer Move | Unit::Assign_Destination, Foot::Mission_Enter, OVER_OUT/UNLOADED/RUN_AWAY und lokale Locomotor-Power-Behandlung erhalten. NavCom, ArchiveTarget und `field_20C` nicht mit einem zweiten Ground-State duplizieren |
| Stop mitten auf dem Depot | Externes ts-patches enthält einen konkreten historischen Explosions-/Scatter-Fix bei 0x00651D0B..0x00651DF7. In den aktuellen Vinifera-Sources wurde kein gleichadressierter Patch gefunden; seine Installation darf nicht unterstellt werden. Das belegt ein relevantes Abort-/Occupation-Risiko, keinen neu durch D4 beobachteten Fehler |

Die Referenz-Patches und die nativen Cleanup-Funktionen sind statische Belege,
keine Runtime-Bestätigung aller Kombinationen. Insbesondere nicht behaupten,
EMP, Capture, Stop, Teleport und volle Ausgangsblockade seien durch D4a getestet.
Ein späterer Adapter muss diese bestehenden Pfade verwenden und darf keine
verdeckten zweiten Besitzerreferenzen erzeugen. Keine Untersuchung einer separaten
Load-/Shutdown-Problematik ist Bestandteil dieser Research.

## 9. Save/Load und CRC

Native Daten decken den aktiven Einzelkontakt bereits ab:

- RadioClass::Radio auf beiden Seiten, Old-Nachrichten und IsTethered;
- CurrentMission, SuspendedMission, MissionQueue, Status und Mission-Timer;
- Unit.NavCom (+278h), ArchiveTarget (+E8h), Nearby-/`field_20C`, weitere
  Navigations-/Pfadzustände und Locomotion;
- Building-Stage `field_2C0`, IsReadyToCommence, Animation/BState und Kontakt;
- Unit.Strength, Geld/House und Locomotor-Ziel/HeadTo/Power-/Trackzustände.

**Radio::Load 0x005BDAA0 fordert ausdrücklich das Swizzle des Zeigers bei +78h
über 0x0060DA70 an.** Object::Load 0x005866F0 ist der zugrundeliegende native
Ladepfad. Foot::Load/Save 0x004A5F40/0x004A61B0, Building::Load/Save
0x00438060/0x00438210 und Unit::Load/Save 0x00659580/0x00659740 decken die
abgeleiteten Daten und den Locomotor-Vertrag ab. Keine neue Ground-Liste ist nötig,
um eine laufende Reparatur darzustellen. Ein nativer Repair-Save kann strukturell
fortgesetzt werden; dies ist hier keine selbst ausgeführte Save/Load-Erprobung.

Für D4b wäre neuer **Typzustand** nötig: mindestens ein Set/Valid-Vertrag und ein
relatives Cell-X/Y auf BuildingTypeExt. Daraus kann der Weltpunkt jedes Mal aus
Building-Anchor und gespeichertem Typ abgeleitet werden. BuildingExt und UnitExt
brauchen für diese empfohlene Einpunktlösung keinen neuen Laufzeitzustand,
Service-Timer, Slotindex oder zusätzlichen Besucherzeiger.

Die Typ-Konfiguration ist dennoch persistenzrelevant: BuildingTypeExt meldet
`sizeof(*this)` als raw Save-Größe, nutzt NoInit beim Load und CRC für Foundation-
Zusatzdaten. Neue Typfelder müssen gespeichert, geladen/validiert und mit
Set/X/Y sowie getrennter Domäne deterministisch gehasht werden; die vorhandene
Foundation-Sync-CRC-Invalidierung ist zu berücksichtigen. Cell-POD braucht kein
Pointer-Swizzle. Nach dem Swizzle muss der Typ-/Rollen-/Bounds-Vertrag passen;
vorher keine noch nicht remappten Peers dereferenzieren.

**Kein neuer Runtime-Pointer bedeutet nicht keine Save-Version-Auswirkung.**
Wird das raw Layout von BuildingTypeExt in D4b erweitert, ist eine kontrollierte
neue Save-Basis oder eine ausdrücklich implementierte Migration erforderlich.
Ein Sidecar nur aus erneut gelesener aktueller INI wäre kein gleichwertiger
saved-game-Vertrag. D4a ändert nichts: **0x190000 bleibt bestehen**. Die konkrete
zukünftige Versionsnummer wird hier nicht festgelegt.

## 10. AI-Nutzung

Unit::Mission_Repair ruft nativ `Find_Docking_Bay(Rule->RepairBay, true, false)`
auf und reserviert mit HELLO. Der dritte bool ist im PE ein only-unoccupied-Gate;
die TS++-Parameterbenennung `evenoccupied` ist dafür irreführend. Die tatsächlichen
Instruktionen bei 0x00637F0B..0x00637F18 entscheiden, nicht der Name.

Find_Docking_Bay prüft vorhandenen Typbestand, Haus/Allianz, Limbo, exakte
BuildingType-Identität, ggf. Radio-Freiheit, Ground-Bewegungszone und CAN_LOAD.
Es bewertet die Distanz zum Building und bevorzugt ggf. IsLeader. Der Aufrufer
erhält ein Building-Objekt; der spätere Fahrweg ist derselbe wie beim Spieler.

Eine reine Änderung des Fahrziels lässt aber die **Zonenauswahl am alten Zentrum**
stehen. Für Custom-Servicepunkte muss diese lokale Ground-Kandidatenprüfung
ebenfalls den Punkt verwenden. Ranking kann zunächst bewusst weiter am Building
bleiben, sofern dies als native Auswahlpolicy dokumentiert wird; es darf nicht
als Distanz zur Servicezelle beschrieben werden. Möchte man letzteres, braucht es
einen weiteren lokalen Distanzadapter. Kein globaler Center- oder Suchalgorithmus-
Ersatz und keine Erweiterung auf beliebige UnitRepair-Typen ohne eigene Freigabe.

`UnitRepair=yes` allein trägt einen neuen MYREPAIR-Typ nicht in Rule.RepairBay ein.
Für die native automatische Missionssuche muss die Mod-Konfiguration den passenden
RepairBay-Typ verwenden. Im untersuchten Code gibt es einen expliziten
MISSION_REPAIR-Übergang für einen als IsUseless bewerteten Harvester. Ein allgemeiner
„jede beschädigte AI-Kampfeinheit fährt selbstständig ins Depot“-Scheduler wurde
nicht gefunden und wird nicht behauptet. Team::Regroup bevorzugt zwar Repair Bays
bei der Wahl eines sicheren Sammelortes, liefert aber einen Safety_Point in der
Umgebung; das ist kein eigener Docking-/Repair-Auftrag.

D4b muss Human- und AI-/Script-Enter-Aufträge gemeinsam abdecken, vorhandene
Archive-/Rally-Fortsetzung erhalten und darf keine neue AI-Reparaturstrategie
als Nebenwirkung einführen. Automatische Auswahl und Move-/Stop-Abbruch bleiben
zwingende spätere Runtime-Fälle.

## 11. CustomFoundation, occupied Punkt und BibShape

**Empfehlung: Servicezelle innerhalb der Bounds UND in der tatsächlichen
Occupied-Cell-Maske.** Zur Laufzeit muss dort dasselbe Building als Occupier
gefunden werden. Bounds-only genügt nicht. Der Punkt wird analog D3 rein
geometrisch aus relativer Zelle und Anchor gebildet, z.B. Cell.As_Coord mit
Building-Z; keine XYZ-/ExitCoord-Offsets und kein BuildingShape-Pixeloffset.

Begründung aus dem Code, nicht aus einer Vorliebe für eine Art-API:

1. Native Repair-Autorisierung erlaubt die belegte Partner-Foundation.
2. ON_DEPOT benutzt Cell_Building; ein Loch/Halo ist danach kein Depot.
3. Die native Wiederaktivierung des ausgeschalteten Kunden beim Move sucht das
   Repair-Building in der aktuellen Occupier-Liste.
4. Ankunft, Service und Abfahrt sind eine Beziehung zu diesem Building, keine
   beliebige Nachbarzelle. Halo würde neue Erkennungs-/Lifecycle-Ausnahmen brauchen.

Ein freier Halo-Punkt erleichtert eventuell die reine Anfahrt, ist aber insgesamt
**nicht die minimale oder nativere Lösung**. Er löst die Radio-, Power_On-,
Verkaufs- und Service-Erkennung nicht. Für D4b deshalb kein Halo und keine Löcher
als Servicezellen. Eine flache zusammenhängende, fahrbar angebundene Belegungsmaske
mit erreichbarem Rand und Ausgang ist der sinnvolle erste Scope; Koordinaten-
Validierung allein kann dynamische Blockaden nicht ausschließen.

Der heutige Scope-Validator lehnt IsCanUnitRepair ausdrücklich mit
`dock/refinery/repair contract` ab. D4b müsste eine **eigene eng validierte
Ground-Service-Rolle** freigeben; nicht einfach den Ablehnungsoperator entfernen.
Refinery/Weeder, Factory, UnitReload-/Aircraft-Hybrid, Hospital/Armory, Naval,
Deploy-/Carryall-/Transport-Sonderrollen bleiben getrennt. Normale Aircraft-D3-
Eligibility oder deren Arrays werden dafür nicht umgedeutet.

**BibShape ist Darstellung, keine Cell-Eintrittserlaubnis.** Vinifera zeichnet
Class.BibShape im Building-Draw-Pfad (buildingext_hooks.cpp um 761); die
Serviceposition und Can_Enter_Cell lesen dessen Pixel nicht. Occupy_List und
Cell-Occupier bleiben entscheidend. `IsBibbed` ist davon zu unterscheiden und
kann native Height/Bib-Verträge beeinflussen. Der aktuelle Validator verbietet
IsBibbed generell und erlaubt BibShape nur für die eng freigegebene Aircraft-
Helipad-Rolle. D3b wird hier nicht verändert. Für den ersten D4b-Scope beide
Freigaben unverändert lassen und Repair-BibShape separat entscheiden; aus der
Helipad-Freigabe folgt keine Ground-Freigabe. Optisch übermalte Fahrwege sind
zudem kein Beweis für passierbare oder unbesetzte Zellen.

## 12. Vergleich mit TD/RA, RA2/YR und Erweiterungen

TD und RA zeigen die erkennbare Herkunft: STRUCT_REPAIR, gegenseitiges Radio,
Foot::Mission_Enter und eine Building-Reparatur mit INITIAL/IDLE/DURING,
NEED_TO_MOVE und RADIO_REPAIR. Tether ist ein gesonderter generischer Mechanismus.
Diese veröffentlichten Quellen sind für das Verständnis nützlich, aber nicht
als direkte TS-Patches übertragbar: ältere TD-/RA-Abstände sind z.B. 0x10,
Abschluss/Abfahrt und Animations-/Timersteuerung unterscheiden sich. In der
lokalen ursprünglichen RA-Unit::Mission_Repair steht sogar ein STRUCT_REFINERY-
Suchargument; daraus darf kein TS-RepairBay-Vertrag abgeleitet werden. Die
Remastered-Quellen bestätigen die Verwandtschaft, ersetzen aber nicht das TS-PE.

**RA2 und YR besitzen tatsächlich einen allgemeinen Repair-Docking-Offset-Pfad.**
Der Vergleich ist nicht auf Aircraft-Docking begrenzt:

| Beleg | Statisches Ergebnis |
|---|---|
| RA2 game.exe: Dock-Koordinatenpfad 0x004440E0 | Bei UnitRepair (Flag +1349h) geht 0x004441A9 in den NumberOfDocks-/DockingOffsets-Pfad. Das Flag ist über den UnitRepair-String/INI-Read bei 0x0045A248..0x0045A25C unabhängig bestätigt |
| RA2 0x004441C9..0x00444283 | Docks=0 -> normale Koordinate; Docks=1 -> Offset[0]; mehrere -> FindLinkIndex(Docker) 0x00636E70, passender Offset oder Fallback |
| YR gamemd.exe: Building::GetDockCoords 0x00447B20 | UnitRepair +16A9h bei 0x00447D06 führt ebenso zu NumberOfDocks +1780h und Offset-Items +1788h |
| YR 0x00447D3D..0x00447DF6 | 0/1/N-Verzweigung, FindLinkIndex 0x0065AD90, Position plus XYZ-Offset des passenden Links; `ret 8` für Ergebnispuffer und Docker |
| YR Building::DistanceToDockingCoord 0x00447E00 | Holt Besucherkoordinate und GetDockCoords(Building, Besucher), dann Distanz. Repair-IDLE benutzt diesen Helper bei 0x0044C1D1 |
| Ground-Reparaturzweige | RA2 UnitRepair ab 0x0044765E, YR ab 0x0044BBC8. Die untersuchten Kundenabfragen greifen mit Index 0 über GetNthLink (RA2 0x00636E10, YR 0x0065AD30) zu; ein gemeinsamer Status/Stage steuert den UnitRepair-Zweig |
| YR RadioClass | Native RadioLinks mit FindLinkIndex/ContainsLink/HasFreeLink/SetLinkCount; dies ist bereits ein anderes Basisklassen-/ABI-System als TS |

Damit ist **ein konfigurierbarer Ground-Repair-Docking-Punkt belegt**. Mehrere
reservierbare Radio-Links und slotbezogene Offsets belegen aber **keinen robusten
parallelen Mehrfahrzeug-UnitRepair-Zyklus**: der untersuchte Ground-Zweig verarbeitet
Link 0 mit einem gemeinsamen Zustand, keine unabhängigen Ground-Reparaturtimer.
NumberOfDocks einfach auf TS zu übertragen würde weder alle Ankunfts- und Exit-
Verbraucher noch einen Multi-Service-Vertrag lösen. Kein nachgespielter
RA2-/YR-Multifahrzeugtest wird behauptet.

Ares/Antares ersetzen den UnitReload-Zweig bei 0x0044C844 und iterieren
RadioLinks mit unabhängigen DockReloadTimers. Die Kommentare beschreiben
Aircraft-Rearm. Das ist kein Ground-Reparatur-Adapter. Antares korrigiert außerdem
Foot::CanBeSold bei 0x004D9EE1 auf GetDockCoords(pThis); das stützt gerade die
Notwendigkeit, Verkauf und Servicepunkt konsistent zu behandeln.

Phobos erweitert Repairrate (UnitRepair-Hook 0x0044BD38), Steps/Prozent/Kosten
(Techno-RX 0x006F4D1A) und unterstützt Repair auf UnitReload-Docks. Die lokale
Dokumentation beschreibt Units.RepairRate/Step/Percent/UseRepairCost. Diese
Ökonomie-Erweiterungen sind kein Bedarf für D4b und sollen nicht portiert werden.
In den untersuchten Ares-/Antares-/Phobos-Stellen wurde kein direkt übernehmbarer
TS-CustomFoundation-Ground-ServiceCell-Adapter gefunden.

DTA-Vinifera enthält denselben wesentlichen skalaren Repair-RX-/Rally-Vertrag.
ts-patches enthält aircraft_repair.asm (0x0040B917 -> 0x0040B929, Aircraft-Depot-
Eignung) und den genannten Depot-Stop-Fix, aber keinen gefundenen Custom-
Ground-Servicepunkt. Self-Heal-/C4-/Healer-Patches sind davon getrennt.
redalert2-recomp liefert im lokalen Checkout keine fertigen gelifteten Engine-
Funktionskörper. Deshalb wurde aus ihm keine Reparaturfunktion erfunden oder ein
Lift erzeugt; die Vergleichsbelege kommen aus den vorhandenen PEs und YRpp.

Übertragbar ist das **Prinzip eines besucherbezogenen Servicepunkt-/Distanzhelpers**.
Nicht übertragen werden RadioLinks-Basisklassenport, YR-Adressen, Dock-Arrays,
Tank-Bunker-Quadranten, Grinding/UnitAbsorb, per-slot XYZ oder Aircraft-Rearm-Timer.

## 13. D3/D3c1-Wiederverwendung einzeln bewertet

A = vorhandenen Mechanismus direkt nutzen; B = Konzept mit eigener Ground-
Konfiguration/Validierung; C = für den empfohlenen D4b-Einpunktvertrag ungeeignet.
B bedeutet hier nicht automatisch neuen persistenten Ground-Besucherzustand.

| Bestandteil | Bewertung | Begründung |
|---|---|---|
| AircraftDockVisitors[] | C | AircraftClass*-Array, weiterer Besitzer neben nativem Radio, für einen Ground-Kunden unnötig |
| Slot reservation | C für D3c1-Implementation; A für natives HELLO | Ground hat schon die passende Einzelreservierung |
| First-Free | C | Ein einziger skalarer Partner, kein Slotscan nötig |
| Coordinate helper | B; A für Cell->Coord-Arithmetik | D3-Pattern Anchor+Cell+Z passt, Guard/Rolle und Ground-Verbraucher unterscheiden sich |
| Service timers | C | Native Ground-Stage/URepairRate/Repair_Cost erhalten, keine Aircraft-ReLoad-Deadlines |
| Radio asymmetry | C | D3c1 benötigt Arraygegenstelle; Ground bleibt gegenseitig skalar |
| Targeted release | A für natives OVER_OUT an richtigen Partner | Keine D3c1-Arrayfreigabe oder künstliche RX-Gegenstelle übernehmen |
| Save/Load | B | Native Kontakt-/Missiondaten nutzen; neue Typkonfiguration sauber in eigenes Layout aufnehmen |
| Swizzle | A für native Radio-/Foot-Zeiger | Keine AircraftVisitor-Remaps für Units, kein neuer Pointer im empfohlenen Adapter |
| CRC | A für POD-/stable-ID-Prinzip, B für neue Typkonfiguration | Kein Cast von Aircraft-IDs und keine Pointerwerte hashen |
| Diagnostics | A für Developer-only-/Transition-Pattern | Nur später passende Ground-Labels und Ereignisse; D4a fügt keine Logs hinzu |

## 14. Hook-/ABI-Audit und bestehende Konflikte

Alle folgenden Bytes wurden am TS-input.dat geprüft oder sind bei den ausdrücklich
genannten vorhandenen Hooks als Originalbytes dokumentiert und gegengeprüft.
Es wird **kein Hook installiert**. Interne Callsite-Kandidaten sind noch kein
freigegebener Patchplan. Die endgültigen Guards, Stolen-Bytes-Wrapper und
Register-/Flag-Erhaltung müssen in D4b implementierungsbezogen geprüft werden.

Koordinatenmethoden: natives __thiscall, ECX=this, versteckter Coord*-Ergebnispuffer
als Stackargument, EAX=derselbe Puffer, callee `ret 4`. Eine nachgeschaltete
Ergebnisanpassung ändert weder NavCom noch den Stack. An einem Call selbst muss
dessen Stackverbrauch exakt emuliert werden. Keine `ret`-Adresse mit noch aktivem
Prolog-Frame anspringen. Process benutzt zudem einen **ILocomotion-Teilzeiger**,
While_Moving den tatsächlichen DriveLocomotionClass-Zeiger.

| Stelle / Zweck | Originalinstruktionen, geprüfte Spanne | Register / Stack / Return / Konflikt |
|---|---|---|
| Building Destination_Coord 0x0042F3B0, Alternative | `8B 81 20 02 00 00` mov eax,[ecx+220h], 6 | ECX=Building, original ESP+4=Coord*. Nicht bevorzugt: kein Besucherargument; globale Typ-/Center-Substitution kann Aircraft und unbeteiligte Aufträge beeinflussen |
| Foot-Zielresultat 0x004A4AD9, bevorzugt wiederverwenden | `8B 30 8B 78 04`, 5 | EBP=Foot, EAX=Coord*, native Zielcall schon fertig. Anpassung vor Replay, weiter 4A4ADE. **D3c1-Hook bereits vorhanden**, Ground-Zweig in denselben Adapter integrieren statt konkurrierendem Hook |
| Drive-Process-Zielresultat 0x0047E1B7 | `8B 08 8B 50 04`, 5 | ESI=ILocomotion-Teilzeiger, Owner=[ESI+4], echter Loco=ESI-4; EAX=Coord*. Native Call 47E1B4 fertig. Replay ->47E1BC, kein Extra-Stackargument. Vorhandener Wake-Patch bei 47DFDB ist getrennt |
| Drive-Ankunftsziel 0x0047F857 | `8B C8 8B 01 99`, 5 | EBP=echter Drive*, LinkedTo=[EBP+8], EAX=Ziel-Coord*. Vor Replay Punkt anpassen, weiter 47F85C. Den Owner-eigenen Z-Call 47F8A7 nicht ersetzen |
| Unit-Ankunft 0x00651356 | `8D 44 24 2C 50`, 5 | EBP=Unit, EBX=Kontakt, Buffer=ESP+2Ch. Custom kann nur Building-Zielpunkt liefern und bis 651360 hinter mov ecx,ebx / Center-Call springen; dann EAX=Buffer, kein übersprungener Push bleibt liegen. Andere Bedingungen/IM_IN beibehalten |
| Guard-Distanz 0x004302DF | `8B 06 51 8B CE`, 5 | ESI=Building, ECX=Ergebnispuffer (ESP+28h), EDI=Besucher-Coord*. Custom Point -> EAX, weiter 4302E7 nach Center-Call; den übersprungenen Push nicht ausführen |
| Repair INITIAL 0x00432446 | `51 8B CD FF 50 4C`, 6 | EBP=Building, ECX=Buffer ESP+3Ch, ESI=Besucher-Coord*, EDI=Toleranz. Weiter 43244C mit EAX=Buffer, kein eigener Stackverbrauch. Native Nähe-/Statuslogik bleibt |
| Repair IDLE 0x00431F26 | `8D 44 24 3C 8B CD`, 6 | EBP=Building, ESI=Kunde, EDI=Kunden-Coord*. Buffer ESP+3Ch; custom weiter431F30 nach Push/Call, EAX=Buffer, kein nachträgliches ret-Emulieren |
| Depot-Verkauf 0x00431025 | `8B 06 51 8B CE`, 5 | ESI=Building, ECX=Buffer ESP+90h, EDI=Kunden-Coord*. Custom weiter43102D mit EAX=Buffer. HIGH: Entscheidung Unit vs Building erhalten |
| Unit Can_Demolish Partner-Center 0x004A5280 | `FF 52 4C 8B 16`, 5 | ECX=Partner, ESI=Foot, [ESP]=Coord*-Argument. Call poppt 4, danach EDX=[ESI], weiter4A5285/EAX=Coord*. Ein Ersatz muss **beides** einschließlich Stackcleanup nachbilden; nicht nur drei Callbytes überschreiben |
| Capture Dockpoint 0x0042F820 | `8D 44 24 38 50`, 5 | ESI=Building, EBX=Kunde, Buffer ESP+38h. Custom weiter42F82D nach mov ecx,esi / Dockcall, EAX=Buffer, kein Pushrest. Keine pauschale Capture-Policy ändern |
| AI-Kandidatenzone 0x00637F30 | `8D 44 24 24 50`, 5 | ESI=Sucher, EDI=Building, Buffer ESP+24h. Custom weiter637F3A nach mov ecx,edi / Center-Call; EAX=Buffer. Nur passende Ground-Kandidaten. Bestehende F0B-/FW-Trace-Hooks im selben Funktionskörper beachten |
| Optionales AI-Distanzranking 0x00637FF2 | `57 8B CE E8 C6 EC F4 FF`, 8 | ESI=Sucher, EDI=Kandidat; push edi / mov ecx,esi / Distance_To. Return EAX=int-Distanz bei637FFA, Argument vom nativen Helper verbraucht. Nicht nötig, wenn bewusst natives Center-Ranking erhalten wird |
| Find_Exit_Cell 0x00434140 | `83 EC 18 53 8B D9`, 6 | __thiscall ECX=Building, original ESP+4=Cell*, ESP+8=const Techno*. EAX=Cell*, ret8. **D1-Hook bereits vorhanden**; Ground-Eligibility getrennt ergänzen, Single-Infantry unverändert. Custom ohne Prolog nutzt nur ret8 bei4342EB |
| Unit Can_Enter_Cell 0x00655330 | `81 EC 90 00 00 00` sub esp,90h, 6; Ausnahmen bei65575C/65583E, s. Abschnitt5 | __thiscall ECX=Unit, Stack+4=Cell*, +8=Facing, +Ch=Level, +10h=andere Cell*, +14h=bool; EAX=MoveType. Für D4b keine neue pauschale Passability-Patchstelle empfohlen |

Bestehende Integrationen, die nicht doppelt installiert/umgangen werden dürfen:

| Adresse | Original / ABI / aktueller Zweck |
|---|---|
| 0x004268C0 | `83 EC 44 53 55`, erste 5 Bytes; __thiscall ECX=Building, ESP+4=from, +8=message, +Ch=long&, EAX=RadioMessageType, retCh. Bereits ganzer RX-C++-Ersatz, also dort lokalen Ground-DOCKING-Distanzfall ergänzen, kein zweiter Entry-Hook |
| 0x00431880 | `83 EC 54 53 55`, 5; ECX=Building, keine expliziten Argumente, EAX=Missiondelay. D3c1 fängt nur Aircraft-MultiDock ab, sonst native Replay; vorhandener Custom-Return4318DA ist bare ret. Ground nicht in Aircraft_Dock_Service umleiten |
| 0x00432184 | Original `8B 45 78 8B 55 00`, erste 6 Bytes (mov eax,[ebp+78h] / mov edx,[ebp]); EBP=Building, ESI=Kunde, kein neuer Funktionsargument-Stack. Bestehender Vinifera-Hook ersetzt 432184..432202, weiter4324DF für Missionsdelay; Exit/Rally bei bereits fehlendem Reparaturbedarf |
| 0x00431DAB | Dieselben ersten 6 Originalbytes `8B 45 78 8B 55 00`; EBP=Building, ESI=Kunde, kein neuer Funktionsargument-Stack. Bestehender Ersatz431DAB..431E27, ErfolgEAX=true ->431E27, Fehler->431E90. Native Ground-Abfahrt erhalten, nicht die nur-Aircraft-CAP-Sonderbehandlung übernehmen |
| 0x0043266C / 0x004325FE | Original jeweils 6 Bytes: `8B 0D 88 C4 74 00` (mov ecx,[Rule]) bzw. `DD 80 A8 0C 00 00` (fld qword [eax+CA8h]). EBP=Building, EAX=Aircraft-Reloadzeit ->43260F; kein neuer Argument-Stack. #204 ReloadRate-Integration im UnitReload-Zweig, keine Ground-URepairRate-Hooks |
| 0x00439D10 / 0x0042C340 | Bestehende C++-Entry-Ersätze für Can_Have_Rally_Point und Assign_Rally_Point. Depot-Rally ist bereits erlaubt, kein eigener D4-Ordertyp nötig |
| 0x00434000 / 0x00436410 | Bestehende Detach_All-/Detach_Anim-Ersätze: Radio-Cleanup bzw. Repair-Animationen mitberücksichtigen |
| 0x0042F2C0 | D3-Hook, `83 EC 10 56 8B F1`, 6; ECX=Building, hidden Coord* ESP+4, EAX=Buffer, ret4. Nur bestehende Aircraft-Eligibility; nicht ungeprüft Ground in den globalen Dock-Override mischen |
| 0x00637F0B | `8A 44 24 48 84 C0`, 6; ESI=Techno, EDI=Kandidat, ESP+48h=only-unoccupied; Aircraft-Sonderfall, Ground bleibt native Replay |
| 0x00637E93 / EAB / F13 / FE0 / 63800D / 638013 | Bestehende Developer-FW-Suchdiagnostik. Keine konkurrierenden Hooks oder Änderungen ihrer Zähler/Guards erforderlich |
| 0x0043FF40 / 0x0043FF50 | CF-Width/Height-Adapter; liefern große Bounds, ändern aber ExitList[Size] nicht |
| 0x0042FE70 | Bestehender Building-Can_Enter_Cell-Ersatz betrifft Platzierung/Unlimbo des Gebäudes, ist nicht Unit::Can_Enter_Cell und kein Ersatz für Fahrberechtigung |

Bei bestehenden Whole-Range-/Entry-Ersätzen sind dies keine Vorschläge für neue
gleichadressierte Trampoline. Die source-definierten Längen 0 bzw. Patch_Jump
sind nicht automatisch eine sichere neue Detourlänge. Für jede tatsächlich
gewählte spätere Mutation sind vollständige Grenzen und Wrapper nochmals zu
prüfen. Die obige neue Callsite-Auswahl zeigt die wesentlichen konkreten
Koordinatenstellen; UI-/Rally-/Cleanup-Code soll soweit möglich unverändert bleiben.

## 15. Architekturvergleich und API-Vorschlag

| Option | Machbarkeit / Änderungen | Reservation / Persistenz | Bewertung |
|---|---|---|---|
| A Coordinate Override Only | Nur Docking_Coord oder Destination_Coord ersetzen lässt PCP, Guard, Repair, Sell/Capture, Zone und Exit inkonsistent. Globaler Center-Ersatz verändert zudem Waffen/UI | Einzelkontakt bliebe, falsche Arrival-/Servicezustände trotzdem möglich | Für das volle Ziel **nicht ausreichend** |
| B Lokaler Custom-Service-Adapter | Ein gemeinsamer Punkthelper; besucherbezogene Foot-/Drive-Ergebnisse, Ankunfts-/Distanz-/Capture-/Sell-/AI-Zonenadapter und großer Exit. Native Radio-/Mission-/Repair-Logik bleibt | Nur neue Typkonfiguration; keine Ground-Runtime-Pointer/Timer | **Empfohlen**, klar begrenzter Scope |
| C BuildingExt GroundVisitor | Zweiter Besitznachweis zusätzlich zu Radio; benötigt Detach/Capture/Limbo/Abort, Swizzle, CRC und konsistente Neuaufnahme | Ohne nachgewiesene Lücke im Einzelkontakt unnötiger persistenter State | Für D4b nicht erforderlich, nicht vorab einführen |
| D Multi-Service | Separate Groundslots/-timer, Koordinaten, Occupation, Anfahrt/Exit und Konkurrenz; Aircraft-Arrays nicht casten | Neue Runtime-Strukturen und umfassender Save-/CRC-Vertrag | LARGE, ausdrücklich nicht D4b |

Vorläufige API-Empfehlung zur Diskussion, **noch kein endgültiger INI-Key**:

| Kandidat | Klarheit / Zukunft / Verwechslungsrisiko |
|---|---|
| ServiceCell=X,Y | Allgemein verständlich, verspricht aber mehr als UnitRepair; später auch Reload/andere Dienste möglich |
| RepairCell=X,Y | Kurz, kann mit Building-Selbstreparatur oder Reparaturziel verwechselt werden |
| UnitRepairCell=X,Y | **Bevorzugter Vorschlag**: ordnet den Punkt eindeutig UnitRepair zu, klar getrennt von Aircraft-DockingCell# |
| DockingCell=X,Y | Verwechslungsgefahr mit bestehenden Aircraft DockingCell0..N-1 und Single-Dock-Alias; nicht für Ground umdeuten |
| ServiceCell0=X,Y | Signalisiert schon jetzt Multi-Service und Slotsemantik, die D4b nicht anbietet; vorerst nicht nötig |

Ein späteres Multi-Service kann eine indizierte Form plus kompatible Zuordnung
des bisherigen Einzelpunkts zu Index0 bekommen. Das rechtfertigt weder ein
heutiges Array noch einen vorzeitig festgelegten NumberOfServices-/Queue-Vertrag.
AF-0005-Konsistenz bedeutet Art-relative Cell-Koordinaten, validierte Maske und
deterministische Fallbacks; es verlangt nicht, Ground als Aircraft-Docking auszugeben.

## 16. Konkrete empfohlene D4b-Grenze

1. BuildingTypeExt speichert genau einen optionalen relativen Ground-Servicepunkt
   mit sauberem Set/Valid-Vertrag. Eigene Eligibility: reine UnitRepair-Rolle,
   gültige CustomFoundation, occupied Punkt in Bounds, keine Hybrid-/Sonderrollen.
   Eine angeforderte Custom-Repair-Rolle darf nicht mit ungültigem/fehlendem Punkt
   als große aktive Maske am alten Zentrum weiterlaufen. Der Invalid-/Fallback-
   Vertrag muss zusammen mit der Eligibility festgelegt werden, vorzugsweise
   Ablehnung der Custom-Rolle mit bestehender sicherer nativer Foundation-Fallback.
2. BuildingExt speichert **keinen** GroundVisitor, UnitExt **keinen** Slotindex,
   keine neue Kontakt-/Service-Mission und keine Ground-Reload-Deadline.
3. Ein gemeinsamer read-only Helper berechnet den Weltpunkt. Physischer Service
   bleibt an den richtigen nativen Radio-Partner gebunden; Vorab-Abfragen für
   AI/Koordinaten dürfen nichts reservieren.
4. Zunächst normale DriveLocomotion-Ground-Units, z.B. Wheel/Track auf flacher,
   zusammenhängender, erreichbar angebundener Maske. Hover, Tunnel/Train,
   Subterranean, Carryall-/Teleport-Sonderintegration und Aircraft-Service an der
   neuen CF-Rolle nicht nebenbei behaupten. Die Rollen-/Anfragegrenzen müssen
   ausdrücklich verhindern, dass ununterstützte Besucher in den Custom-Pfad geraten.
5. Für die Anfahrt bevorzugt lokale, **besucherbewusste** Resultatadapter bei
   Foot-Zielaufnahme, Drive-Neuziel und Drive-Ankunft, statt den globalen
   Building-Destination-/Center-Return zu verändern. So bleiben Building-Objekt
   und NavCom erhalten, während alle drei Ground-Koordinatenabfragen zusammenpassen.
6. PCP-Ankunft, Guard, Repair-INITIAL/IDLE und DOCKING-Distanz benutzen denselben
   Punkt. Ebenso die depotbezogenen Can_Demolish-/Sell-/Capture-Prüfungen. Keine
   neue Strength-, Kosten-, Rate- oder Tether-Steuerung.
7. Find_Docking_Bay erhält einen begrenzten Ground-Zonenadapter für den Kandidaten-
   Punkt. Native Typ-/Allianz-/Bestandsauswahl bleibt, Ranking zunächst bewusst
   nativ. Human, AI und Scripts laufen danach über denselben Eingang.
8. Lokaler Find_Exit_Cell-Adapter für die große Repair-Maske; geeigneten äußeren
   Ausgang deterministisch suchen, aktuelle Rally-/Archive-/OVER_OUT-Semantik
   erhalten. Blockierte Ausgänge dürfen keinen falschen Erfolg erzwingen.
9. Native Cell-Eintrittsberechtigung und Locomotor-Architektur erhalten. Falls
   die begrenzte erste Maske damit nicht zuverlässig erreichbar/räumbar ist,
   Scope stoppen und neu bewerten, nicht pauschal alle Occupation-Prüfungen umgehen.
10. Typ-Persistenz/NoInit/CRC/Save-Version bewusst integrieren. Danach später
    Nutzer-Build und manuelle Runtime-Matrix; D4a selbst implementiert davon nichts.

## 17. Entscheidungsmatrix und Risiken

| Bereich | Native Semantik | D4b Änderung nötig? | Risiko |
|---|---|---|---|
| Repair-Bay Detection | UnitRepair, CAN_LOAD, Rule.RepairBay für Suchmission | Eigene CF-Rolle/Anfrage-Eligibility, keine neue AI-Strategie | MEDIUM |
| Reservation | Beidseitiges HELLO, ein skalarer Partner | Nein, keine zweite Reservation | LOW |
| Approach Target | Building als NavCom, wiederholte entry_50 -> Center | Ja, konsistente Ground-Zielresultate | HIGH |
| Cell Entry | Partner-/Repair-Ausnahme in Unit::Can_Enter_Cell | Keine neue globale Passability; Scope/Erreichbarkeit prüfen | HIGH |
| Repair Start | PCP-Zellvergleich, IM_IN, Guard-/Repair-Distanzen | Ja, derselbe explizite Punkt | HIGH |
| Repair Tick | Native Building-Stage, URepairRate, RADIO_REPAIR | Nein | LOW |
| Cost | Typ-Repair_Cost/Step, Geld des Kunden | Nein | LOW |
| Completion | Native Antworten/Status/Animation | Nein, bestehende Abfahrtshelfer weiterbenutzen | LOW |
| Exit | Native ExitList[Size], Rally/Archive | Ja, Ground-Ausgang für große Maske | HIGH |
| Save/Load | Native Radio-/Foot-/Loco-/Stage-Daten | Neue Typkonfiguration/Version/CRC, kein Besucher-Swizzle | MEDIUM |
| AI | Rule.RepairBay, Center-Zone und Ranking | Lokaler Zonenpunkt, Konfiguration und AI-/Script-Regressionsfälle | HIGH |

| Konkretes Risiko | Priorität | Begrenzung / offen vor Freigabe |
|---|---|---|
| Servicepunkt nicht betretbar / Occupation blockiert Driver | HIGH | Richtiger Radiopartner, occupied Punkt, flach/erreichbar, kein neues allgemeines MOVE_OK |
| Fahrzeug bleibt im Building stecken | HIGH | Alle drei Zielverbraucher und großer äußerer Exit, Native Power_On/Abbruch erhalten |
| Zwei Kunden kollidieren | MEDIUM | Ein Kontakt, kein neues Parallelservice; native räumliche Konkurrenz bleibt |
| Falscher Radio-Partner / stale Reservation | HIGH | Native gegenseitige Identität, keine Sidecar-Besitzer, Detach/OVER_OUT erhalten |
| Building zerstört / verkauft / captured | HIGH | Nativer Cleanup plus servicepunktbezogene Sell-/Capture-Entscheidung |
| Stop / Missionabort / neuer Move | HIGH | Historischen Depot-Abort-Fall nicht übersehen; kein Re-Enter durch Adapter |
| Save während Repair | HIGH | Native Missions-/Loco-/Stage-Daten und neues Typ-Save zusammen validieren |
| Bounds / Maskenlöcher / abgetrennte Komponenten | HIGH | Nicht Bounds-only erlauben; erste Rolle eng begrenzen |
| BibShape / IsBibbed | MEDIUM | Render von Occupation trennen, D3b-Freigabe nicht automatisch ausweiten |
| Pathfinding-Schleifen | HIGH | Wiederholte Driver-Abfragen konsistent, echte Blockade nicht als Ankunft melden |
| AI bzw. Human-Abweichung | HIGH | Rule.RepairBay und Zonenwahl, nicht nur manuelle Anfahrt prüfen |
| Rally-/Archive-/Move-Interferenz | MEDIUM | Vorhandene Vinifera-Abfahrtshelfer nutzen, keine neue Order-Queue |
| EMP / Energie / IonStorm | MEDIUM | Unterschiedliche native Flags/Power-Zustände erhalten; spätere Runtime-Abdeckung erforderlich |

**Umfang MEDIUM** für die genannten Einschränkungen: keine neue Ökonomie,
kein Ground-Slot-Subsystem, kein eigener Driver, aber mehrere native
Koordinatenstellen, Exit- und Save-Integration. **LARGE** würde es bei beliebigen
Masken/Locomotoren, freiem Halo, Multi-Service, ATC/Queue oder Portierung von
YR-RadioLinks. Diese Erweiterungen nicht in einen kleinen D4b-Auftrag verstecken.

**GO WITH RESTRICTIONS**, nicht uneingeschränkt GO und keine Runtime-Zusage.
Vor der Implementation explizit festlegen/prüfen:

- Erster unterstützter Fahrzeug-/Locomotor-Scope und Ablehnung von Aircraft,
  Hover-/Tunnel-/Transport-/Carryall-Sonderfällen an der Custom-Rolle.
- Flat-/Masken-/Erreichbarkeitsvertrag einschließlich eines tatsächlich freien
  äußeren Ausgangs; Verhalten bei vollständig blockiertem Perimeter.
- Endgültige Auswahl der lokalen Hookresultate samt Flags/Stack-/COM-Teilzeigern;
  keine konkurrierenden D1-/D3c1-Trampoline.
- AI-Kandidatenzone zur Servicezelle, bewusstes Ranking und Rule.RepairBay-
  Konfiguration; generelle AI-Autoreparatur ist kein D4b-Feature.
- Verkauf/Capture desselben Kunden am neuen Punkt sowie Stop/Move-Abbruch;
  historischer Depot-Explosionspfad ist noch keine für diesen Stand getestete Zusage.
- Vollständige Power-/EMP-/IonStorm- und Save-während-Anfahrt/Service/Abfahrt-
  Abdeckung bei der späteren Runtime-Erprobung; keine Pause-Policy neu erfinden.
- Neuer Typ-Save-/CRC-Vertrag und passende zukünftige Save-Basis, API-Name erst
  im freigegebenen D4b-Design verbindlich machen.

## 18. Abschluss dieses Research-Auftrags

Erstellt wird ausschließlich diese Datei. Keine Sourceänderung, keine installierten
Hooks, kein Build, kein Test/Runtime-Test, kein Recomp-Lift, kein git add,
kein Commit, kein Push. docs/Whats-New.md bleibt unverändert. Referenz-Repositories
wurden ausschließlich gelesen. Die Research-Datei bleibt untracked/unstaged.
Abschlussprüfung: `git diff --check` ohne Whitespacefehler. Git zeigt eine neue
untracked Datei nicht im normalen `git diff -- <datei>` an; der angeforderte
Dateidiff ist deshalb leer. Die neue Datei wurde zusätzlich direkt geprüft und
mit `git diff --no-index --check -- /dev/null docs/AF-0005D4a-Research.md` ohne
Whitespacefehler untersucht. Exit1 dieses no-index-Diffs bezeichnet die neue,
abweichende Datei; Git meldet zusätzlich nur die vorhandene LF/CRLF-Policy.
Kein Staging ist dafür nötig. Finaler Repository-Status:

```text
?? docs/AF-0005D4a-Research.md
```
