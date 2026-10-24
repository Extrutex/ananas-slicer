# Validierung des Spannungslösers (analyze.stress)

Stand: 24.10.2026, Branch `feat/ui-stage5-kraft`, Löser unverändert (`src/engine/Stress.cpp`). Alle Aussagen sind
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

Auswertung: Das Maximum sitzt in allen Stufen **in der Zelle an der inneren Ecke (8 | 8)** (Abstand höchstens eine Zellgröße), die
Armspannung und die Durchbiegung stimmen mit der Balkentheorie (−0,5 % bzw. innerhalb der Schranken). Die Spitzenspannung an der Ecke
**wächst bei jeder Halbierung etwa um das 1,3- bis 1,4-fache**, hat also keinen Grenzwert. Der Sicherheitsfaktor an der Ecke ist
daher netzabhängig (2,9 bis 1,2). Er ist **als Warnung zu lesen** („hier ist Spannung konzentriert“), nicht als Zahl: Wer an einer
scharfen Innenecke eine Sicherheit braucht, muss die Ecke ausrunden. Gegenüber der Balkenabschätzung liegt die Größenordnung bei
0,7- bis 1,75-fach, je nach Auflösung; Faktor 2 Abweichung in beide Richtungen sollte man für scharfe Ecken einplanen.

## Befunde am Löser (nicht behoben)

Kein Fall zeigt einen Fehler im Gleichungslöser, der nicht durch die Voxelauflösung erklärbar wäre. Zwei Auffälligkeiten der
Abbruchlogik bei feinen, schlanken Gittern, beide **Verdacht (Vermutung), nicht bewiesen**:

1. **Vorzeitiger Abbruch „stagniert“** (Stagnationsfenster 1000 Iterationen, `src/engine/Stress.cpp` Zeile 986 ff.):
   - Eingabe: Balken 96 × 6 × 6 mm, E = 2000, ν = 0,3, Linienlager unten an x = 0 und x = 96, Last 50 N auf einem 2 mm breiten
     Streifen oben in der Mitte, Voxel 0,25 mm (384 × 24 × 24 Zellen).
   - Erwartet: Konvergenz wie bei den Stufen 1,0 / 0,5 / 0,4 / 0,3 mm derselben Geometrie (1 184 / 2 458 / 3 233 / 4 411 Iterationen,
     Durchbiegung 4,278 … 4,343 mm). Hochgerechnet etwa 5 300 Iterationen.
   - Erhalten: Fehler „the solver stagnated after 2000 iterations (relative residual 0.844286)“.
   - Zweiter Fall: Platte 40 × 100 × 2 mm (und 40 × 80 × 2 mm) mit Bohrung d = 10 mm, F = 800 N, Voxel 0,25 mm: „stagnated after 4000
     (bzw. 3000) iterations (relative residual 0.00339 bzw. 0.00606)“, während Voxel 1,0 / 0,5 nach 2 766 / 5 702 Iterationen konvergieren.
   - Deutung (Vermutung): Das Residuum des CG-Verfahrens mit Jacobi-Vorkonditionierer verharrt bei Biegung über einige tausend
     Iterationen auf einem Plateau und fällt erst danach; die Regel „je 1000 Iterationen mindestens 1 % besser“ bricht dann ab, bevor
     das Plateau endet. Die Lagerung ist in Ordnung (Starrkörperprüfung bestanden, gröbere Stufen konvergieren). Für Nutzer heißt das:
     bei feinen Gittern kann ein berechenbarer Fall mit der irreführenden Meldung „Lager lassen den Körper frei“ abbrechen.
   - Umgehung in den Tests: Stufen so gewählt, dass sie konvergieren.
2. **Iterationszahl wächst etwa mit 1/Voxelgröße** (Platte 2 673 → 5 458 → 6 822 bei 0,5 / 0,25 / 0,2 mm), dazu passt die
   Obergrenze `STRESS_MAX_ITERATIONS = 20000`. Das ist erwartet für CG mit Jacobi, gehört aber zur Aussage „feines Gitter ist teuer“.

## Offen

- Kein unabhängiges FE-Programm als Gegenprobe (nicht installiert). Die Formeln sind Idealisierungen (ebener Spannungszustand,
  Balkentheorie); ein Teil der gezeigten Abweichungen (Biegung L/h = 8) ist Modellunterschied, nicht Lösergenauigkeit.
- Torsion eines Kreisstabs nicht abgebildet (Begründung oben).
- Das Verhalten für noch feinere Gitter an der Platte mit Bohrung (Überschwingen) und die Wirkung einer Ausrundung an der
  Winkelinnenecke wurden nicht gemessen.
- Reale Druckteile sind anisotrop und haben Wand-/Füllungsstruktur; die Fälle hier sind massive, homogene Körper.
