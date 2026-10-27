import { useLayoutEffect, useRef } from "react";
import { EmptyState } from "../../components/EmptyState";
import { Panel } from "../../components/Panel";
import { formatTimeOfDay } from "../../lib/format";
import { useChat } from "../../state/ServerState";
import styles from "./ChatPanel.module.css";

// Within this many pixels of the bottom counts as reading the newest line.
const pinSlackPx = 8;

/** Global chat as it is relayed, in a fixed-height log that follows new lines unless scrolled up. */
export function ChatPanel() {
  const lines = useChat();
  const log = useRef<HTMLDivElement>(null);
  const pinned = useRef(true);

  useLayoutEffect(() => {
    const element = log.current;
    if (element !== null && pinned.current) element.scrollTop = element.scrollHeight;
  }, [lines]);

  const onScroll = () => {
    const element = log.current;
    if (element !== null) pinned.current = element.scrollHeight - element.scrollTop - element.clientHeight <= pinSlackPx;
  };

  return (
    <Panel title="Chat" description="Global channel, since this page opened" flush>
      <div ref={log} className={styles.log} role="log" aria-label="Global chat" tabIndex={0} onScroll={onScroll}>
        {lines.length === 0 ? (
          <div className={styles.empty}>
            <EmptyState icon="chat" title="No messages yet">
              Lines players send to the Global channel appear here while this page is open.
            </EmptyState>
          </div>
        ) : (
          lines.map((line) => (
            <div key={line.seq} className={styles.line}>
              <time className={`${styles.time} tabular`} dateTime={new Date(line.atMs).toISOString()}>
                {formatTimeOfDay(line.atMs)}
              </time>
              <p>
                <span className={styles.name} title={`Player ${line.playerId}`}>
                  {line.name}
                </span>{" "}
                {line.text}
              </p>
            </div>
          ))
        )}
      </div>
    </Panel>
  );
}
