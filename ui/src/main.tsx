import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App.tsx";
import { EngineClient } from "./engine/client.ts";
import { createTransport } from "./engine/transport.ts";
import { CockpitSession } from "./state/session.ts";
import "./styles.css";

const session = new CockpitSession(new EngineClient(createTransport()));
void session.start();

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <App session={session} />
  </StrictMode>,
);
