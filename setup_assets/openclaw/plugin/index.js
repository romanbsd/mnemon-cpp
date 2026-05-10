import { existsSync, unlinkSync, writeFileSync } from "fs";
import { homedir } from "os";
import { join } from "path";

const COMPACT_FLAG = join(homedir(), ".mnemon", ".compact-pending");

export default function register(api) {
  // api.pluginConfig holds plugins.entries.mnemon.config from openclaw.json
  const cfg = api.pluginConfig ?? {};
  const remind  = cfg.remind  !== false; // default on
  const nudge   = cfg.nudge   !== false; // default on
  const compact = cfg.compact === true;  // default off

  // ── before_compaction (void) ──────────────────────────────────
  // Cannot inject context directly — write a flag file instead.
  // before_prompt_build will pick it up on the next turn.
  if (compact) {
    api.on("before_compaction", async () => {
      try {
        writeFileSync(COMPACT_FLAG, String(Date.now()), "utf-8");
      } catch {
        // ignore write failure
      }
    });
  }

  // ── before_prompt_build ───────────────────────────────────────
  // Always load the mnemon skill; optionally inject remind/nudge/compact hints.
  api.on("before_prompt_build", async () => {
    const parts = ["[mnemon] load mnemon skill"];

    // Compact flag: was compaction triggered since last turn?
    if (compact && existsSync(COMPACT_FLAG)) {
      try { unlinkSync(COMPACT_FLAG); } catch { /* ignore */ }
      parts.push(
        "[mnemon] Context compaction starting. Review this session and remember the most valuable insights (up to 5) before context is compressed. Delegate to Task sub-agents now."
      );
    }

    if (remind) {
      parts.push(
        "[mnemon] Evaluate: recall needed? After responding, evaluate: remember needed?"
      );
    }

    if (nudge) {
      parts.push(
        "[mnemon] Consider: does this exchange warrant a remember sub-agent?"
      );
    }

    return { prependContext: parts.join("\n\n") };
  });
}
