import { useEffect, useState } from "react";
import { api } from "../../api/client";
import type { Marker } from "../../api/types";

// A recording's markers do not change once it is finished, and several decks
// may show the same one, so fetch each once per page load.
const cache = new Map<string, Promise<Marker[]>>();

function loadMarkers(recordingId: string): Promise<Marker[]> {
  let markers = cache.get(recordingId);
  if (markers === undefined) {
    markers = api.recordings.get(recordingId).then((detail) => detail.markers);
    markers.catch(() => cache.delete(recordingId)); // let a later attempt retry
    cache.set(recordingId, markers);
  }
  return markers;
}

/** Timeline markers of a recording; empty until loaded, and if loading fails. */
export function useMarkers(recordingId: string): Marker[] {
  const [loaded, setLoaded] = useState<{ recordingId: string; markers: Marker[] } | null>(null);

  useEffect(() => {
    let cancelled = false;
    loadMarkers(recordingId).then(
      (markers) => {
        if (!cancelled) setLoaded({ recordingId, markers });
      },
      () => undefined,
    );
    return () => {
      cancelled = true;
    };
  }, [recordingId]);

  return loaded?.recordingId === recordingId ? loaded.markers : [];
}
