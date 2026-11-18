import { useEffect, useState } from "react";

/** The tokens a canvas chart needs, resolved, since a canvas cannot read CSS variables. */
export interface ChartTheme {
  series: [string, string, string];
  text: string;
  muted: string;
  faint: string;
  grid: string;
  axis: string;
  surface: string;
  font: string;
}

function read(): ChartTheme {
  const style = getComputedStyle(document.documentElement);
  const token = (name: string) => style.getPropertyValue(name).trim();
  return {
    series: [token("--series-1"), token("--series-2"), token("--series-3")],
    text: token("--text"),
    muted: token("--text-muted"),
    faint: token("--text-faint"),
    grid: token("--border"),
    axis: token("--border-strong"),
    surface: token("--surface"),
    font: token("--font"),
  };
}

/** Follows the system color scheme, which the tokens switch on. */
export function useChartTheme(): ChartTheme {
  const [theme, setTheme] = useState(read);
  useEffect(() => {
    const query = window.matchMedia("(prefers-color-scheme: light)");
    const update = () => setTheme(read());
    query.addEventListener("change", update);
    return () => query.removeEventListener("change", update);
  }, []);
  return theme;
}
