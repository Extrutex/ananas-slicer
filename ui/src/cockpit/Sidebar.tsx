import { useCallback, useId, useMemo, useState } from "react";
import { countStates, sharedSource, type CockpitGroup } from "./model.ts";
import { ParamRow, ROW_GRID } from "./ParamRow.tsx";

interface GroupSectionProps {
  group: CockpitGroup;
  expanded: boolean;
  rejected: Readonly<Record<string, string>>;
  onToggle: (groupId: string) => void;
  onValueChange: (paramId: string, value: number) => void;
  onDismiss: (paramId: string) => void;
}

function GroupSection({ group, expanded, rejected, onToggle, onValueChange, onDismiss }: GroupSectionProps) {
  const regionId = useId();
  const { exceeded, caution } = countStates(group.params);
  const source = sharedSource(group.params);

  return (
    <section data-testid={`group-${group.id}`}>
      <h3 className="m-0">
        <button
          type="button"
          aria-expanded={expanded}
          aria-controls={regionId}
          onClick={() => onToggle(group.id)}
          className="flex h-row-header w-full items-center gap-2 border-b border-edge bg-surface-raised px-3 text-left transition-colors duration-fast hover:bg-surface-hover"
        >
          <span
            aria-hidden="true"
            className={`inline-block w-3 text-ks-micro text-ink-muted transition-transform duration-fast ${expanded ? "rotate-90" : ""}`}
          >
            {"▸"}
          </span>
          <span className="text-ks-ui font-semibold text-ink">{group.title}</span>
          <span className="font-data text-ks-table text-ink-muted">{group.params.length}</span>
          <span className="ml-auto flex min-w-0 items-center gap-1.5">
            {source ? (
              <span className="truncate font-data text-ks-table text-ink-muted" title="Limit source">
                {source}
              </span>
            ) : null}
            {/* A collapsed group still shows its violation count: collapsing must never hide a limit breach. */}
            {exceeded > 0 ? (
              <span className="shrink-0 rounded bg-signal-limit px-1.5 font-data text-ks-micro font-semibold text-ink-on-signal">
                {exceeded} OVER
              </span>
            ) : null}
            {caution > 0 ? (
              <span className="shrink-0 rounded border border-signal-caution px-1.5 font-data text-ks-micro font-semibold text-signal-caution">
                {caution} NEAR
              </span>
            ) : null}
          </span>
        </button>
      </h3>
      <div id={regionId} role="group" aria-label={group.title} hidden={!expanded}>
        {group.params.map((param) => (
          <ParamRow key={param.id} param={param} rejected={rejected[param.id]} onValueChange={onValueChange} onDismiss={onDismiss} />
        ))}
      </div>
    </section>
  );
}

export interface SidebarProps {
  groups: CockpitGroup[];
  /** key -> the engine's reason for refusing an input. */
  rejected: Readonly<Record<string, string>>;
  onValueChange: (paramId: string, value: number) => void;
  onDismiss: (paramId: string) => void;
  /** e.g. "Voron 2.4 350 0.4 nozzle" */
  machineLabel: string;
  /** e.g. "printer.cfg · printer.cfg" or "profile defaults" */
  configSource: string;
}

export function Sidebar({ groups, rejected, onValueChange, onDismiss, machineLabel, configSource }: SidebarProps) {
  const [collapsed, setCollapsed] = useState<ReadonlySet<string>>(() => new Set());

  const toggle = useCallback((groupId: string) => {
    setCollapsed((previous) => {
      const next = new Set(previous);
      if (next.has(groupId)) next.delete(groupId);
      else next.add(groupId);
      return next;
    });
  }, []);

  const totals = useMemo(() => countStates(groups.flatMap((group) => group.params)), [groups]);

  return (
    <aside aria-label="Parameters" className="flex h-full w-sidebar shrink-0 flex-col border-r border-edge bg-surface-panel font-ui text-ink">
      <header className="border-b border-edge px-3 py-2.5">
        <h2 className="m-0 text-ks-section font-semibold text-ink-strong">Kinematics</h2>
        <p className="m-0 mt-0.5 truncate font-data text-ks-table text-ink-muted" data-testid="machine-line">
          {machineLabel} {"·"} {configSource}
        </p>
        <p
          role="status"
          aria-live="polite"
          data-testid="status-line"
          className={`m-0 mt-2 text-ks-table ${totals.exceeded > 0 ? "text-signal-limit" : totals.caution > 0 ? "text-signal-caution" : "text-ink-muted"}`}
        >
          {totals.exceeded > 0
            ? `${totals.exceeded} parameter${totals.exceeded === 1 ? "" : "s"} exceed${totals.exceeded === 1 ? "s" : ""} a machine limit. Export requires explicit acknowledgement.`
            : totals.caution > 0
              ? `All values within limits · ${totals.caution} within 10 % of a limit.`
              : "All values within limits."}
        </p>
      </header>

      <div
        aria-hidden="true"
        className={`${ROW_GRID} h-6 border-b border-edge bg-surface-panel pl-4 pr-3 font-ui text-ks-micro uppercase text-ink-muted`}
      >
        <span>Parameter</span>
        <span className="text-right">Value {"·"} unit</span>
        <span className="text-center">Step</span>
        <span className="text-right">Limit</span>
      </div>

      <div className="min-h-0 flex-1 overflow-y-auto">
        {groups.map((group) => (
          <GroupSection
            key={group.id}
            group={group}
            expanded={!collapsed.has(group.id)}
            rejected={rejected}
            onToggle={toggle}
            onValueChange={onValueChange}
            onDismiss={onDismiss}
          />
        ))}
      </div>

      <footer className="border-t border-edge px-3 py-2 font-data text-ks-micro text-ink-muted">
        <kbd>{"↑"}</kbd>/<kbd>{"↓"}</kbd> step {"·"} <kbd>Shift</kbd> {"×"}10 {"·"} <kbd>Alt</kbd> {"×"}0.1 {"·"}{" "}
        <kbd>Enter</kbd> apply {"·"} <kbd>Esc</kbd> revert
      </footer>
    </aside>
  );
}
