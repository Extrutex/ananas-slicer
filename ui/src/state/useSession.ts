import { useSyncExternalStore } from "react";
import type { CockpitSession, CockpitState } from "./session.ts";

export function useSession(session: CockpitSession): CockpitState {
  return useSyncExternalStore(session.subscribe, session.getState);
}
