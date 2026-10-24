// Texte der Kraftverteilung, die an mehreren Stellen gebraucht werden.

/** Der Hinweis, der neben jedem Ergebnis steht (docs/HLSD/engine.md, „Stress analysis“, Grenzen). */
export const INFILL_WARNING =
  "Infill und Wände sind nicht modelliert: Das Teil wird als Vollkörper gerechnet. Das ist eine Obergrenze der Steifigkeit; Spannung, Sicherheit und Verformung sind Richtwerte, keine Garantie. Spannungsspitzen an Ecken und Lastpunkten wachsen mit feinerem Gitter.";

/** Der Hinweis unter jedem Ergebnis (docs/HLSD/stress-validierung.md, Winkelhalter). */
export const CORNER_NOTE =
  "Spannungsspitzen an scharfen Innenecken und an Lastpunkten sind netzabhängig (Singularität): Sie wachsen mit feinerem Gitter und haben keinen Grenzwert. Den Sicherheitsfaktor dort nur als Warnung lesen („hier ist Spannung konzentriert“), nicht als Zahl. Eine Ausrundung der Ecke begrenzt die Spitze.";
