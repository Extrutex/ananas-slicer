# Validierung des Spannungslösers (analyze.stress)

Stand: 24.10.2026, Branch `feat/ui-stage6-multigrid`; der Löser rechnet mit einem Multigrid-Vorkonditionierer (Abschnitt „Multigrid-Vorkonditionierer“); die Spalten „Iterationen“ in den Fallabschnitten (a) bis (e) sind die Werte mit **Jacobi** (`preconditioner: "jacobi"`), mit Multigrid stehen sie in der Vorher/Nachher-Tabelle. Alle Aussagen sind
**Richtwerte, nicht zertifiziert**. Der Löser ist linear-elastisch, rechnet mit trilinearen Hexaeder-Elementen auf
einem Würfelgitter (Voxel) und gibt die Spannung als Wert im Elementmittelpunkt aus.

Referenzprogramme (CalculiX, Gmsh, SfePy) sind auf dem Mac mini **nicht installiert**; es wurde nichts nachinstalliert.
Alle Referenzen sind deshalb geschlossene Formeln mit Quelle. Die Fehler sind relativ zur Formel; die Formel ist selbst
eine Idealisierung (siehe Spalte „Grenze der Referenz“ in den Abschnitten).

Die Tests liegen in `tests/engine/test_stress_validation.cpp` (Catch2, Namen beginnen mit „Stress-Validierung“).
Die feinsten Stufen (Minuten Rechenzeit) laufen nur mit `ANANAS_STRESS_FULL=1`:

```
cd build/mini
ctest -C Release -R "Stress|engine|Limits"
ANANAS_STRESS_FULL=1 ./tests/engine/Release/engine_tests.app/Contents/MacOS/engine_tests "Stress-Validierung*"
```

Werkstoff in allen Fällen (außer d): E = 2000 MPa, Querkontraktion 0,3, Streckgrenze 40 MPa. Einheiten mm, N, MPa.

## Übersicht

| Fall | Referenz (Quelle/Formel) | Ergebnis (grob → fein) | Fehler % (grob → fein) | Aussage |
|---|---|---|---|---|
| (a) Platte mit Bohrung, d/W = 0,25, Spannungsüberhöhung Kt | Peterson/Pilkey, Diagramm 4.1: Ktg = 3,2271 | Kt 2,647 → 3,025 → 3,120 | −18,0 → −6,3 → −3,3 | Unterschätzt, konvergiert von unten. Richtwert, nicht zertifiziert |
| (b) Dreipunktbiegung L/h = 16, Durchbiegung | Timoshenko 4,3177 mm | 4,278 → 4,330 → 4,343 mm | −0,9 → +0,3 → +0,6 | Gut. Richtwert, nicht zertifiziert |
| (b) dito, Randfaserspannung Feldmitte | M c / I | 26,8 → 29,9 → 31,1 MPa | −3,6 → −2,2 → −1,8 | Leicht zu niedrig. Richtwert, nicht zertifiziert |
| (b) Dreipunktbiegung L/h = 8, Durchbiegung | Timoshenko 1,1177 mm | 1,133 → 1,153 → 1,163 mm | +1,4 → +3,2 → +4,1 | Wächst mit Verfeinerung, siehe Deutung. Richtwert, nicht zertifiziert |
| (b) dito, Randfaserspannung Feldmitte | M c / I | 26,2 → 29,3 → 30,9 MPa | −5,8 → −4,0 → −3,4 | Leicht zu niedrig. Richtwert, nicht zertifiziert |
| (c) Rohr unter Innendruck, Radialverschiebung nahe Bohrung (Achse) | Lamé (ebener Spannungszustand) | — | +1,9 → −0,3 → +0,7 | Gut. Richtwert, nicht zertifiziert |
| (c) dito, mittlere Vergleichsspannung r = 12 / 15 / 18 mm | Lamé | — | (+3,5/+3,0/+2,3) → (+2,0/+1,7/+1,4) → (0,0/+0,2/+0,2) | Konvergiert sauber. Richtwert, nicht zertifiziert |
| (c) dito, größte Verschiebung | Lamé an der Bohrung 0,0492 mm | 0,0541 → 0,0543 → 0,0511 mm | +10,1 → +10,4 → +3,9 | Örtliche Treppenzelle, zu hoch. Richtwert, nicht zertifiziert |
| (c) dito, größte Vergleichsspannung | Lamé an der Bohrung 11,67 MPa | 11,9 → 12,5 → 13,8 MPa | +1,9 → +7,3 → +18,0 | **Wächst** mit der Verfeinerung (Treppenkanten), nicht verwenden |
| (d) Zugprobe in z statt x, Dehnungsverhältnis | 1 / z_factor | exakt | 0,000 (z_factor 0,3 / 0,5 / 0,8) | Bestätigt. Richtwert, nicht zertifiziert |
| (d) dito, Sicherheitsverhältnis z / x | z_factor | exakt | 0,000 | Bestätigt |
| (e) Winkelhalter, größte Vergleichsspannung | Balken: 19,7 MPa (Sicherheit 2,03) | 13,9 → 19,7 → 26,2 → 34,6 MPa | Verhältnis 0,71 → 1,00 → 1,33 → 1,75 | Maximum an der inneren Ecke, **ohne Grenzwert** (Singularität) |
| (e) dito, Armspannung bei x = 24 | Balken (Zellmitte) | — | −6,7 → −2,7 → −1,2 → −0,5 | Konvergiert zur Balkentheorie |
| (e) dito, Armende-Durchbiegung | Balken 1,487 … 2,489 mm (Schranken) | 1,849 → 1,918 → 1,942 → 1,951 mm | innerhalb der Schranken | Plausibel |

Kurz: **Verschiebungen und Felder fern von Kanten stimmen auf wenige Prozent**, die Konvergenz ist glatt. **Spitzenwerte
an Kerben und Kanten** sind die Schwachstelle: an glatt gerundeten Kerben werden sie mit grobem Gitter **unterschätzt**
(Platte mit Bohrung), an Treppenkanten, Linienlagern und scharfen Innenecken **wachsen** sie mit der Verfeinerung ohne
Grenzwert (Rohr, Lagerkante, Winkelhalter).

## (a) Platte mit Mittelbohrung unter Zug

- Geometrie: W = 24 mm, L = 48 mm, t = 2 mm, Bohrung d = 6 mm (d/W = 0,25), Bohrung als 240-Eck. Zug F = 480 N,
  Bruttospannung 10 MPa. Linke Seite gleitet in ihrer Ebene, drei Punktlager gegen Starrkörperbewegung. Last gleichmäßig
  auf der rechten Stirnfläche.
- Referenz: Peterson, *Stress Concentration Factors* (Pilkey, 2. Aufl.), Diagramm 4.1, Fassung nach Howland/Heywood:
  Ktn = 3,00 − 3,140 (d/W) + 3,667 (d/W)² − 1,527 (d/W)³ = 2,4203, bezogen auf die Nettospannung F / ((W − d) t).
  Auf die Bruttospannung bezogen: Ktg = Ktn / (1 − d/W) = **3,2271**. (Eine zweite Schreibweise mit 3,13 / 3,66 / 1,53
  weicht um 0,08 % ab.) Die Quelle wurde nur über Sekundärseiten gelesen (z. B. amesweb.info, fchartsoftware.com), nicht
  im Buch selbst.
- Grenze der Referenz: Peterson gilt für den ebenen Spannungszustand (dünne Platte). Hier ist t/d = 0,33, die dreidimensionale
  Dickeneinwirkung kann einige Prozent ausmachen. Die Last greift als Flächenkraft an (nicht als starre Einspannung), was
  zu Petersons Annahme gleichmäßiger Zugspannung passt.

| Voxel (mm) | Zellen je Bohrungsradius | Zellen gesamt | Kt (brutto) | Fehler % | Iterationen |
|---|---|---|---|---|---|
| 0,50 | 6 | 17 984 | 2,647 | −18,0 | 2 673 |
| 0,25 | 12 | 143 872 | 3,025 | −6,3 | 5 458 |
| 0,20 | 15 | 280 840 | 3,120 | −3,3 | 6 822 |

Auswertung: Das Voxelgitter **unterschätzt** die Kerbspannung, weil die Spannung im Elementmittelpunkt (eine halbe Zelle
vom Rand entfernt) ausgegeben wird und die Treppenstufen den Rand glätten. Der Fehler fällt etwa mit der Zellgröße
(−18 → −6,3 → −3,3 %), die Reihenfolge ist monoton. Für ein verlässliches Kt braucht es **mindestens etwa 12 Zellen je
Bohrungsradius** (Fehler dann um −6 %, auf der unsicheren Seite). Bei noch feinerem Gitter ist wegen der Treppenkanten
(Singularität) irgendwann ein Überschwingen zu erwarten; das wurde nicht untersucht.

Zum Vergleich ein Feldwert: in der Ligamentmitte (r/a = 3) liegt die Vergleichsspannung bei 10,6–10,7 MPa; die Kirsch-Lösung
der unendlichen Platte (10,08 MPa) gilt wegen der endlichen Breite dort nicht, das ist kein Löserfehler.

## (b) Dreipunktbiegung

- Balken 48 × 6 × 6 mm (L/h = 8) und 96 × 6 × 6 mm (L/h = 16, Last halbiert, gleiche Randfaserspannung). Linienlager unten an
  beiden Enden (links x, y in den Ecken gegen Verschieben und Drehung um z), Last in der Mitte auf einem 2 mm breiten
  Streifen oben.
- Referenz Timoshenko: δ = F L³ / (48 E I) + F L / (4 κ G A), κ nach Cowper = 10 (1 + ν) / (12 + 11 ν) = 0,8497.
  Randfaser: σ = (F L / 4) · c / I im Zellmittelpunkt der untersten Schicht (c = h/2 − Voxel/2).

| L/h | Voxel (mm) | Zellen über Höhe | Durchbiegung (mm) | Fehler % | Randfaser (MPa) | Referenz (MPa) | Fehler % | Iterationen |
|---|---|---|---|---|---|---|---|---|
| 16 | 1,0 | 6 | 4,278 | −0,9 | 26,78 | 27,78 | −3,6 | 1 184 |
| 16 | 0,5 | 12 | 4,330 | +0,3 | 29,88 | 30,56 | −2,2 | 2 458 |
| 16 | 0,3 | 20 | 4,343 | +0,6 | 31,10 | 31,67 | −1,8 | 4 411 |
| 8 | 1,0 | 6 | 1,133 | +1,4 | 26,18 | 27,78 | −5,8 | 539 |
| 8 | 0,5 | 12 | 1,153 | +3,2 | 29,32 | 30,56 | −4,0 | 1 142 |
| 8 | 0,25 | 24 | 1,163 | +4,1 | 30,86 | 31,94 | −3,4 | 2 348 |

Auswertung: Die **Durchbiegung wächst** mit der Verfeinerung und überschreitet die Balkentheorie (bis +4,1 % bei L/h = 8).
Das ist kein Löserfehler: Die Balkentheorie enthält die örtliche Eindrückung unter dem Lastband und über den Linienlagern nicht,
die dreidimensionale Rechnung schon. Dieser absolute Anteil (rund 0,04 mm) wird bei dem schlankeren Balken (L/h = 16) relativ
kleiner (+0,6 %). Das grobe Gitter wirkt der Verfeinerung entgegen (Schubsperren, zu steif), deshalb stimmt es dort „zufällig“
besser. Die Randfaserspannung in Feldmitte ist stets etwas zu niedrig (−2 bis −6 %) und nähert sich der Theorie.
Das **globale Maximum** der Vergleichsspannung liegt am Lagerrand bzw. unter dem Lastband und ist **kein** Feldwert: bei
L/h = 8 und 0,25 mm steigt es auf 42,2 MPa (Kante des Linienlagers, Singularität), während die Feldspannung 30,9 MPa beträgt.

## (c) Dickwandiges Rohr unter Innendruck

- Innenradius a = 10 mm, Außenradius b = 20 mm, Länge 8 mm, Druck p = 5 MPa, freie Enden. Der Druck wird als 32 Teillasten auf
  die Dreiecke der Bohrung gelegt (je Sektor Kraft = −p · Summe der Flächenvektoren, also die exakte Druckresultierende).
  Gelagert wird nur in Punkten, an denen die rotationssymmetrische Lösung keine Verschiebung hat (Mittelhöhe, Symmetrieachsen),
  damit die Lager die Lösung nicht stören. Die Summe von Last und Lagerkräften ist 0 (geprüft).
- Referenz Lamé im ebenen Spannungszustand (σz = 0, exakt für Druck nur auf der Bohrung): u_r(r) = p a² / (b² − a²) ·
  ((1 − ν) r + (1 + ν) b² / r) / E; σθ(r) = p a² / (b² − a²) · (1 + b²/r²); σr(r) = p a² / (b² − a²) · (1 − b²/r²).
  An der Bohrung: u_r = 0,0492 mm, σθ = 8,33 MPa, Vergleichsspannung 11,67 MPa.
- Die Torsion eines Kreisstabs wurde **nicht** untersucht: Sie braucht ein Moment, die Lastauswahl der Engine liefert nur
  Kraftvektoren je Auswahl. Mit mehreren tangentialen Teillasten ließe es sich nachbilden, bringt aber über Fall (c) hinaus
  keine neue Aussage zum Löser.

| Voxel (mm) | Zellen über Wand | u_r Achse | Mittel-σ_v r = 12 / 15 / 18 | größte Verschiebung | größte σ_v |
|---|---|---|---|---|---|
| 1,0 | 10 | +1,9 % | +3,5 / +3,0 / +2,3 % | 0,0541 mm (+10,1 %) | 11,89 MPa (+1,9 %) |
| 0,5 | 20 | −0,3 % | +2,0 / +1,7 / +1,4 % | 0,0543 mm (+10,4 %) | 12,51 MPa (+7,3 %) |
| 0,25 | 40 | +0,7 % | 0,0 / +0,2 / +0,2 % | 0,0511 mm (+3,9 %) | 13,76 MPa (+18,0 %) |

Auswertung: Das **Feld** (Verschiebung auf der Achse, Vergleichsspannung gemittelt über den Umfang) konvergiert glatt auf 0,2 %.
Die **Einzelspitzen** nicht: Die größte Verschiebung liegt in einer einzelnen vorspringenden Treppenzelle an der Bohrung (z. B.
bei (25 | 28 | 8), unter 58° zur Achse, an der Endfläche), und die größte Vergleichsspannung sitzt an einer Treppenkante der
gekrümmten Bohrung am Rohrende (13,8 MPa bei (16,1 | 10,6 | 0,1)). Beide sind Gitter-Artefakte der Treppenfläche und
**wachsen mit der Verfeinerung** (Vergleichsspannung +1,9 → +7,3 → +18 %); sie sind nicht zur Auslegung geeignet. An gekrümmten
Flächen mit Druck sollten Nutzer den Feldwert im Bauteil lesen, nicht die Spitzenzahl.

## (d) Anisotropie quer zu den Schichten

Zugprobe 4 × 4 × 20 mm, 100 N, einmal in x, einmal in z gedruckt, z_factor ∈ {0,3; 0,5; 0,8}, Voxel 1,0 und 0,5.
Dehnung aus der mittleren Verschiebung der ersten und letzten Zellschicht.

| z_factor | Dehnung x (σ/E) | Dehnung z | Verhältnis (Soll 1 / z_factor) | Sicherheit z / x (Soll z_factor) |
|---|---|---|---|---|
| 0,3 | 3,125e−3 | 1,0417e−2 | 3,33333 (3,33333) | 0,30000 |
| 0,5 | 3,125e−3 | 6,250e−3 | 2,00000 (2,00000) | 0,50000 |
| 0,8 | 3,125e−3 | 3,906e−3 | 1,25000 (1,25000) | 0,80000 |

Abweichung 0,000 % (Voxel 1,0 und 0,5 identisch). Der Zustand liegt im Elementraum, es bleibt nur die Löser-Toleranz.
Hinweis: Das Verhältnis der **größten Verschiebungen** (3,322 / 1,995 / 1,248) liegt knapp unter 1 / z_factor, weil dieser Wert
auch die Querkontraktion enthält (Wurzel aus Längsdehnung² + Querkontraktion²); für den Vergleich ist die Dehnung maßgeblich.

## (e) Winkelhalter

- L-Profil, Arm 50 mm lang, Schenkel 8 mm dick, 10 mm breit, senkrechter Schenkel 40 mm hoch mit einer Bohrung (d = 3 mm,
  Achse in Breitenrichtung). Rückwand x = 0 fest (alle Achsen). Last 50 N nach unten auf der Stirnfläche des Arms.
- Balkenabschätzung: Biegespannung an der inneren Ecke σ = 6 F l / (b t²) mit l = 42 mm (Arm ab innerer Ecke) = **19,7 MPa**,
  Sicherheit 40 / 19,7 = **2,03**. Durchbiegung am Ende als Kragarm mit Schub: untere Schranke 1,487 mm (l = 42 mm, Ecke
  starr), obere Schranke 2,489 mm (l = 50 mm, Wandeinspannung).
- Grenze der Referenz: Die scharfe Innenecke ist eine Spannungssingularität, die Balkentheorie sieht sie nicht. Eine Ausrundung
  (Radius ≥ 2 mm) würde die Spitze begrenzen; dieses Modell hat keine.

| Voxel (mm) | Zellen über Armdicke | Zellen | größte σ_v (MPa) | Ort | σ_v / Balken | Sicherheit (Löser) | Durchbiegung (mm) | Arm x = 24 vs. Balken |
|---|---|---|---|---|---|---|---|---|
| 2,0 | 4 | 800 | 13,89 | (9,0 \| 7,0 \| 3,0) | 0,71 | 2,88 | 1,849 | −6,7 % |
| 1,0 | 8 | 6 520 | 19,65 | (8,5 \| 7,5 \| 2,5) | 1,00 | 2,04 | 1,918 | −2,7 % |
| 0,5 | 16 | 51 840 | 26,16 | (8,25 \| 7,75 \| 4,75) | 1,33 | 1,53 | 1,942 | −1,2 % |
| 0,25 | 32 | 415 360 | 34,55 | (8,12 \| 7,88 \| 4,88) | 1,75 | 1,16 | 1,951 | −0,5 % |

Hinweis im Programm: Unter dem Ergebnis im Panel Kraftverteilung steht, dass Spannungsspitzen an scharfen Innenecken und an Lastpunkten
netzabhängig sind (Singularität) und der Sicherheitsfaktor dort nur als Warnung zu lesen ist.

Auswertung: Das Maximum sitzt in allen Stufen **in der Zelle an der inneren Ecke (8 | 8)** (Abstand höchstens eine Zellgröße), die
Armspannung und die Durchbiegung stimmen mit der Balkentheorie (−0,5 % bzw. innerhalb der Schranken). Die Spitzenspannung an der Ecke
**wächst bei jeder Halbierung etwa um das 1,3- bis 1,4-fache**, hat also keinen Grenzwert. Der Sicherheitsfaktor an der Ecke ist
daher netzabhängig (2,9 bis 1,2). Er ist **als Warnung zu lesen** („hier ist Spannung konzentriert“), nicht als Zahl: Wer an einer
scharfen Innenecke eine Sicherheit braucht, muss die Ecke ausrunden. Gegenüber der Balkenabschätzung liegt die Größenordnung bei
0,7- bis 1,75-fach, je nach Auflösung; Faktor 2 Abweichung in beide Richtungen sollte man für scharfe Ecken einplanen.

## Multigrid-Vorkonditionierer

Das CG-Verfahren wird mit einem geometrischen Mehrgitter-V-Zyklus vorkonditioniert (Aufbau und Gründe in `engine.md`, Abschnitt
„Stress analysis“). Gemessen auf dem Mac mini (M4, 12 Kerne), gleiche Toleranz 1e-6, gleiche Fälle wie oben; Rechenzeit ist die der ganzen
Analyse (Voxelisierung, Aufbau der Hierarchie, Lösen, Auswertung). Jacobi-Werte sind die Messungen vor der Änderung.

| Fall | Zellen | Jacobi: Iterationen | Jacobi: s | Multigrid: Iterationen | Multigrid: s | Ebenen | Fehler gegen Referenz (unverändert) |
|---|---|---|---|---|---|---|---|
| (b) Balken L/h 16, 1,0 mm | 3 456 | 1 184 | 0,12 | 20 | 0,02 | 3 | Durchbiegung −0,9 % |
| (b) Balken L/h 16, 0,5 mm | 27 648 | 2 458 | 1,34 | 24 | 0,07 | 4 | +0,3 % |
| (b) Balken L/h 16, 0,3 mm | 128 000 | 4 411 | 10,27 | 27 | 0,26 | 5 | +0,6 % |
| (b) Balken L/h 16, 0,25 mm | 221 184 | 5 669 | 19,78 | 27 | 0,45 | 5 | +0,6 % |
| (b) Balken L/h 8, 0,25 mm | 110 592 | 2 348 | 4,24 | 21 | 0,22 | 5 | +4,1 % |
| (a) Platte mit Bohrung, 0,5 mm | 17 984 | 2 673 | 1,07 | 29 | 0,06 | 4 | Kt −18,0 % |
| (a) Platte mit Bohrung, 0,25 mm | 143 872 | 5 458 | 16,77 | 38 | 0,38 | 5 | Kt −6,3 % |
| (a) Platte mit Bohrung, 0,2 mm | 280 840 | 6 822 | 30,66 | 32 | 0,60 | 5 | Kt −3,3 % |
| (a) Platte 80 × 40 mm, d = 10, 0,25 mm | 399 488 | 11 575 | 80,81 | 38 | 1,09 | 5 | Kt +0,5 % |
| (c) Rohr, 1,0 / 0,5 / 0,25 mm | 7 584 / 60 160 / 482 688 | 96 / 183 / 340 | 0,02 / 0,21 / 2,89 | 9 / 9 / 9 | 0,03 / 0,08 / 0,48 | 3 bis 5 | u_r +1,9 / −0,3 / +0,7 % |
| (e) Winkelhalter, 2,0 / 1,0 / 0,5 / 0,25 mm | 800 bis 415 360 | 158 / 309 / 625 / 1 250 | 0,01 / 0,05 / 0,59 / 8,72 | 10 / 10 / 11 / 11 | 0,01 / 0,02 / 0,07 / 0,38 | 2 bis 5 | größte σ_v 13,9 / 19,7 / 26,2 / 34,6 MPa |
| massiver Würfel 100 mm, 1 mm (Last schräg) | 1 000 000 | 904 | 13,47 | 10 | 0,80 | 6 | — |
| massiver Würfel 170 mm, 1 mm | 4 913 000 | 1 535 | 122,77 | 10 | 4,15 | 6 | — |

Zeilen (a) bis (e): alle Felder und Spitzenwerte der Validierungsläufe stimmen mit denen von Jacobi überein (die Ausgaben
der Validierungstests sind bis auf Gleichstände zwischen spiegelbildlichen Zellen, die 7. Stelle der Dehnung und Kt 3,2422 → 3,2421 bei der großen Platte identisch). Die
Messlatte „Balken 96 × 6 × 6 mm bei 0,25 mm unter 100 Iterationen“ wird mit **27** erreicht (vorher 5 669), die Platte mit Bohrung
bei 0,25 mm (399 488 Zellen) mit **38** (vorher 11 575). Die Rechenzeit sinkt bei den feinsten Gittern um den Faktor 19 bis 74, bei
sehr groben bleibt sie gleich (unter 0,05 s ohnehin).

Speicher am massiven Würfel mit 4,9 Millionen Zellen (`/usr/bin/time -l`, größter residenter Satz): **1,67 GB mit Multigrid, 1,57 GB
mit Jacobi**, also +107 MB (+7 %); die Hierarchie braucht dort 22 Byte je Knoten der feinsten Ebene. Dünne Körper brauchen je Feinknoten mehr, weil
die Knotenzahl je Ebene nur um 1/4 (Platte) oder 1/2 (Stab mit einer Zelle Querschnitt) statt 1/8 fällt; die Schätzung vor dem Aufbau folgt deshalb den
Gitterabmessungen (Test `Stress: the memory estimate of the hierarchy follows the shape of the body`, gemessene Hierarchie gegen Schätzung): Würfel 48 mm
(117 649 Knoten) 11,6 MB gegen 19,8 MB, Platte 120 × 120 × 2 mm bei 0,5 mm (290 405 Knoten) 11,6 gegen 14,2 MB, Stab 200 × 4 × 4 mm bei 0,25 mm 10,2 gegen 13,3 MB,
Stab 300 × 6 × 6 mm bei 0,5 mm 5,2 gegen 6,9 MB, Stab 400 × 1 × 1 mm 11,4 gegen 22,7 MB (dort überwiegt die dichte gröbste Matrix, die Schätzung zählt ihre zwei
Kopien während der Faktorisierung). Jede Ebene wird vor der Allokation gegen das Budget geprüft, ebenso die dichte Matrix samt Faktorkopie. Der
Spitzenspeicher der ganzen Stress-Suite (`/usr/bin/time -l`, mit `ANANAS_STRESS_FULL=1`) liegt bei 1,70 GB gegenüber 1,93 GB vor der Änderung (Messrauschen der Läufe).

**Spektrumschätzung und Prüfung des Zyklus.** Die Glättung braucht λ_max von D⁻¹K. Eine Potenziteration (15 oder 40 Schritte) liegt bis zu 8 % bzw. 5 % darunter;
jetzt liefert ein Lanczos-Lauf (8 bis 30 Schritte) den größten Ritz-Wert (höchstens 2,3 % unter dem wahren Wert, gemessen an 23 Ebenen), gedeckelt durch die
Gershgorin-Grenze (das 1,7-Fache des wahren Werts, als alleinige Schätzung kostet sie 3 bis 5 Iterationen mehr). Ein Sicherheitsfaktor kostet Iterationen, ohne den Zyklus sicherer zu machen
(Regressionsfälle, Iterationen bei Faktor 1,0 / 1,1 / 1,3: Platte 70 / 72 / 72, Kragarm 24 × 4 × 4 8 / 9 / 10, Hohlkasten 16 35 / 36 / 38); der Zyklus bleibt positiv definit, solange
der wahre Wert unter etwa dem 1,3-Fachen des verwendeten liegt (Test mit künstlich verkleinertem λ: Faktor 0,9 und 0,8 laufen mit weniger Iterationen, ab 0,7 ist der Zyklus in allen 15
Fällen nicht mehr positiv definit). Deshalb ist der Faktor 1 und die Absicherung die **Laufzeitprüfung** nach dem Aufbau: mit festen Zufallsvektoren x, y müssen ⟨Mx,x⟩ > 0, ⟨My,y⟩ > 0
und ⟨Mx,y⟩ = ⟨x,My⟩ gelten (Zufallsvektoren enthalten alle Frequenzen, gerade die oberen, die ein zu kleines λ verstärkt); sonst Jacobi mit Warnung. Die Prüfung kostet vier Zyklen.
Gemessen an den 18 Fällen des Operatortests (Regressionsfälle, Lager, nur gröbste Ebene): Asymmetrie höchstens 4,4e-14, ⟨Mx,x⟩/⟨x,x⟩ immer positiv (kleinster Wert 4,8e-4),
|⟨Pu,v⟩ − ⟨u,Rv⟩| / (|Pu||v|) höchstens 1,3e-16 an Rand und Lagern, kleinster/größter Pivot der gröbsten Matrix mindestens 8,2e-7 (Schwelle 1e-12).

**Gleichheit mit Jacobi** (Test `Stress: multigrid gives the Jacobi result …`, gleiche Toleranz 1e-6): größte Abweichung eines
Zellwerts, bezogen auf das Maximum des Felds, bei Verschiebung und Vergleichsspannung höchstens 1,6e-7; Abweichung der größten
Vergleichsspannung höchstens 1,1e-7 (Forderung des Tests: 1e-4). Fälle: Kragarm 24 × 4 × 4, 33 × 17 × 9 (ungerade, nx ≠ ny ≠ nz),
30,5 × 7,5 × 5,5 bei 0,5 mm, Stab 200 × 2 × 2 und 101 × 3 × 3, Platte eine Zelle dick, z_factor 0,3, Lager nur an 2 × 2 Zellen,
drei Punktlager an Knoten mit ungeradem Index, Hohlkasten mit Wand 2 und 1 Zelle, zwei getrennte Balken, L-Form aus zwei Teilen.

| Robustheitsfall | Jacobi: Iterationen | Multigrid: Iterationen |
|---|---|---|
| Kragarm 33 × 17 × 9 (ungerade) | 196 | 11 |
| Stab 200 × 2 × 2 | 2 849 | 11 |
| **Platte eine Zelle dick, 60 × 40 × 1** | 1 495 | **71** |
| z_factor 0,3 | 162 | 12 |
| Lager nur an 2 × 2 Zellen | 532 | 20 |
| drei Punktlager (ungerade Knoten) | 398 | 16 |
| Hohlkasten, Wand 2 Zellen | 436 | 22 |
| **Hohlkasten, Wand 1 Zelle** | 312 | **35** |
| zwei getrennte Balken | 129 | 11 |
| L-Form (leere Grobzellen) | 543 | 15 |

Die schwächsten Fälle sind die mit Wänden von einer Zelle: dort ist die Grobgitter-Näherung (Teilfüllung als Steifigkeitsanteil) am
gröbsten, der V-Zyklus bleibt trotzdem 20- bis 60-mal besser als Jacobi. Die Obergrenze im Test ist 150 Iterationen (großzügig).

**Rückfall:** Scheitert der Aufbau der Hierarchie (zu wenig Speicherbudget, Ebenen werden nicht kleiner) oder wird ein Zyklus
nicht positiv definit/nicht endlich oder wächst das Residuum auf mehr als das 10 000-fache der Last, rechnet der Löser mit Jacobi
weiter (vom aktuellen Stand, bei Aufbaufehler von vorn) und meldet das in `warnings`. Mitten im Lauf wird das CG dabei **neu gestartet** (r bleibt, z mit Jacobi neu, p = z, rz = ⟨r,z⟩):
die alte Fortführung bildete beta = rz_Jacobi / rz_Multigrid aus Skalaren zweier Vorkonditionierer. Gemessen (Ausfall ab dem 3. bzw. 110 − 100 = 10. Zyklus), Iterationen gesamt neu gegenüber alt:
Platte 60 × 40 × 1 mm 1 760 / 1 767 gegenüber 5 060 / 5 035 (Jacobi allein 1 495), Balken 96 × 6 × 6 mm bei 0,5 mm 1 334 / 940 gegenüber 6 116 / 948 (allein 730), Hohlkasten 322 / 291 gegenüber 712 / 345 (allein 312).
Die Zeit wird in jeder Phase geprüft (Spektrumschätzung je Schritt, Aufbau je Ebene und um die Faktorisierung, vor Vor- und Nachglättung); ein bereits konvergiertes Ergebnis wird nie verworfen.
Eine fast singuläre oder nicht positive gröbste Matrix (Pivots unter 1e-12 des größten, fehlerhafte Probelösung) führt ebenfalls zu Jacobi mit Warnung; die Testfälle mit Rollenlagern und drei dicht
beieinander liegenden Punktlagern waren nicht singulär (Pivotverhältnis ≥ 6,6e-5), die Ablehnung ist daher mit dem Testschalter geprüft. Getestet über einen Testschalter
(`fault_for_test`, nicht im Protokoll) und mit einem Speicherbudget, das nur das Jacobi-Gerüst fasst. Bei einer **Stagnation** mit
Multigrid wird nicht auf Jacobi gewechselt (es wäre langsamer); die Stagnationsregel beendet den Lauf wie zuvor mit einer Meldung.
Bei der Abstimmung der Glättung (Polynomgrad 1 bis 4, Bereichsverhältnis 3 bis 8) war ein Parameter versehentlich leer
(Verhältnis 0, der Polynomschritt nicht endlich): der Rückfall griff, der Lauf endete mit Jacobi und dem erwarteten Ergebnis.

## Befunde am Löser

### 1. Vorzeitiger Abbruch „stagniert“ (behoben)

**Ursache bestätigt (Fakt, gemessen):** Die alte Regel in `src/engine/Stress.cpp` verlangte, dass das beste Residuum in jedem
Fenster von 1000 Iterationen um mindestens 1 % fällt. Das Residuum des CG-Verfahrens mit Jacobi-Vorkonditionierer ist nicht
monoton und liegt bei feinen, schlanken Gittern tausende Iterationen auf einem Plateau, bevor es fällt. Dieselben Läufe mit
abgeschalteter Regel konvergierten:

| Fall | alte Regel | ohne Regel / neue Regel | Rechenzeit (Mac mini) |
|---|---|---|---|
| Balken 96 × 6 × 6 mm, Voxel 0,25 (221 184 Zellen) | Abbruch bei 2 000 Iterationen, Residuum 0,844286 (reproduziert, identisch) | **konvergiert nach 5 669 Iterationen**, Residuum 9,6e-7; Durchbiegung +0,6 %, Randfaserspannung −1,8 % gegenüber der Theorie | 20 bis 30 s |
| Platte 80 × 40 × 2 mm, Bohrung d = 10 mm, F = 800 N, Voxel 0,25 (399 488 Zellen) | Abbruch bei 3 000, Residuum 0,006 | **konvergiert nach 11 575 Iterationen**, Residuum 9,9e-7; Kt (brutto) 3,2422 gegen 3,2271 (+0,5 %) | etwa 90 s |
| Platte 100 × 40 × 2 mm, sonst gleich (501 888 Zellen) | Abbruch bei 4 000 (Angabe der ersten Untersuchung) | **konvergiert nach 14 170 Iterationen**, Kt +0,4 % | etwa 2,5 min |

Verlauf des Balkens (Residuum relativ, alle 500 Iterationen das bisher beste): 0,95 (500), 0,95 (1000), 0,66 (1500), 0,66 (2000),
0,60 (2500), 0,19 (3000), 0,024 (3500), 1,5e-4 (4000), 7,5e-6 (4500). Die Platte (80 × 40) hat ein Plateau des besten Residuums von
7 500 bis 9 500 (1,4e-4, 2 000 Iterationen ohne Fortschritt) und konvergiert danach.

**Neue Regel:** Stagnation gilt erst, wenn das beste Residuum über ein langes Fenster um weniger als 1 % gefallen ist, mindestens
3 000 Iterationen **und** mindestens die Hälfte aller bisherigen Iterationen (Prüfung alle 250 Iterationen); das Fenster wächst also
mit dem Lauf. Harte Grenzen bleiben `max_iterations` (Deckel 20 000) und das Zeitlimit. Ein echter Mechanismus (Residuum bleibt auf
einem Boden) endet weiter schnell: der Testfall „zwei Würfel an einer Ecke“ endet nach 800 Iterationen in 0,01 s, weil der Löser dort
schon vorher eine nicht positive Richtung findet (`pq <= 0`). Ein reiner Residuumsboden ohne `pq <= 0` wurde nicht als eigener Test
nachgestellt (nicht herstellbar ohne künstliches Gleichungssystem); die Regel dafür ist nur durch die Messungen oben begründet.

**Meldung bei Nichtkonvergenz:** Die Starrkörperprüfung der Lager läuft vorab. Wer danach nicht konvergiert, bekommt nicht mehr
„die Lager lassen den Körper frei“, sondern die Iterationen, das Residuum und die Toleranz sowie den Hinweis „größere voxel_size, oder
mehr Zeit und Iterationen“ (Iterationsdeckel), „größere voxel_size“ und Fugen prüfen (Stagnation, Mechanismus) bzw. Werkstoff/Lasten prüfen (Überlauf).

**Iterationsdeckel 20 000:** reicht für alle gemessenen Fälle (größter: 14 170). Die Zahl wächst etwa mit 1/Voxelgröße^1,4
(Balken 4 411 → 5 669 von 0,3 auf 0,25 mm). Hochrechnung (**Vermutung, nicht gemessen**): eine dünne Platte an der Zellgrenze von
5 Millionen Zellen bräuchte etwa 22 000 Iterationen und rund 30 Minuten, also über dem Standard von `max_seconds` = 600 und knapp
über dem Deckel. Deshalb **nicht geändert**: Bei solchen Größen entscheidet das Zeitlimit, und der Deckel schützt vor einem Lauf
ohne Ende; die Meldung nennt jetzt beide Wege. Der richtige Hebel war ein besserer Vorkonditionierer: mit dem Multigrid-Vorkonditionierer (Abschnitt oben) braucht
dieselbe Platte 38 statt 11 575 Iterationen, ein 5-Millionen-Zellen-Körper etwa 10 statt über 1 500; die Hochrechnung gilt nur noch für `preconditioner: "jacobi"`.
Ein 3×3-Blockjacobi wurde nicht versucht.

Test: `Stress-Validierung: feine schlanke Gitter konvergieren statt mit Stagnation abzubrechen` (nur mit `ANANAS_STRESS_FULL=1`, etwa
2 Minuten; die Abschnitte „Jacobi“ erzwingen den Jacobi-Vorkonditionierer, damit die Regel weiter geprüft wird, die Abschnitte „Multigrid“ prüfen unter 100 Iterationen).

### 2. Iterationszahl wächst mit 1/Voxelgröße

Platte 48 × 24 × 2 mm: 2 673 → 5 458 → 6 822 Iterationen bei 0,5 / 0,25 / 0,2 mm. Erwartet für CG mit Jacobi, gehört zur Aussage
„feines Gitter ist teuer“; mit Multigrid bleibt die Zahl bei 9 bis 38 (Tabelle oben) und wächst kaum mit der Verfeinerung.

### 3. Auswertung nach dem Löser ohne Uhr (Doku angepasst)

Reaktion, Spannung je Zelle und Extremwertsuche laufen nach dem Löser ohne Zeitprüfung. Gemessen an einem massiven Würfel mit
4 913 000 Zellen (nahe dem Zellenlimit von 5 Millionen): **zusammen etwa 0,1 s** (Reaktion 0,04 s, Spannung 0,02 s, Extrema 0,01 s,
Dünnwandprüfung 0,02 s), gegen etwa 13 s für den Löser mit 172 Iterationen (0,07 s je Iteration). Eine Uhr hier wäre Aufwand ohne
Wirkung; die Grenze ist durch das Zellenlimit nach oben beschränkt. `max_seconds` kann deshalb um bis zu etwa 0,1 s (bei 5 Millionen
Zellen) überschritten werden. In `engine.md` so vermerkt.

### 4. Kantentreffer bei der Strahlparität (nicht bestätigt)

Der Voxelisierer schickt senkrechte Strahlen durch die Zellmitten, um einen festen Bruchteil der Voxelgröße verschoben, damit ein
Strahl nie auf einer Dreieckskante liegt. Gezielte Probe: Platten, deren Ober- und Unterseite in ein Dreiecksnetz zerlegt sind, dessen
Kanten und Ecken genau an den Strahlorten liegen (Float-gerundet, so wie ein Netz sie speichert), mit einer und mit wechselnden
Diagonalen, bei 9 Voxelgrößen (0,1 bis 1,5 mm): **in allen 18 Fällen** korrekte Zellzahl, kein Paritätsfehler. Gegenprobe (nur
zur Messung, wieder entfernt): ohne den Versatz brechen dieselben Netze mit „nicht wasserdicht“ ab, die Probe kann den Fehler also
erkennen. Mit dem Versatz ist ein exakter Treffer nur möglich, wenn eine Float-Koordinate genau dem Double-Strahlort entspricht;
das ist nicht herstellbar. **Nichts geändert**; der Test `Stress: vertical rays that run along or across the edges and corners of the
surface mesh` bleibt als Absicherung. Eine Eckenregel (oben-links) wäre nur nötig, wenn der Versatz je entfällt.

## Offen

- Kein unabhängiges FE-Programm als Gegenprobe (nicht installiert). Die Formeln sind Idealisierungen (ebener Spannungszustand,
  Balkentheorie); ein Teil der gezeigten Abweichungen (Biegung L/h = 8) ist Modellunterschied, nicht Lösergenauigkeit.
- Torsion eines Kreisstabs nicht abgebildet (Begründung oben).
- Das Verhalten für noch feinere Gitter an der Platte mit Bohrung (Überschwingen) und die Wirkung einer Ausrundung an der
  Winkelinnenecke wurden nicht gemessen.
- Reale Druckteile sind anisotrop und haben Wand-/Füllungsstruktur; die Fälle hier sind massive, homogene Körper.
