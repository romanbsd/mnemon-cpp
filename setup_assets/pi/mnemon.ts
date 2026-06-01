import { execFileSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";

function promptDir(): string {
  return join(process.env.MNEMON_DATA_DIR ?? join(process.env.HOME ?? "", ".mnemon"), "prompt");
}

function guidePath(): string | undefined {
  const scoped = join(promptDir(), "guide.md");
  if (existsSync(scoped)) return scoped;

  const legacy = join(process.env.HOME ?? "", ".mnemon", "prompt", "guide.md");
  if (existsSync(legacy)) return legacy;

  return undefined;
}

function readGuide(): string {
  const path = guidePath();
  return path ? readFileSync(path, "utf8") : "";
}

function memoryStatus(): string {
  try {
    const raw = execFileSync("mnemon", ["status"], {
      encoding: "utf8",
      stdio: ["ignore", "pipe", "ignore"],
      timeout: 5000,
    });
    const stats = JSON.parse(raw);
    return `[mnemon] Memory active (${stats.total_insights ?? 0} insights, ${stats.edge_count ?? 0} edges).`;
  } catch {
    return "[mnemon] Memory active.";
  }
}

function visibleMessage(content: string) {
  return {
    customType: "mnemon",
    content,
    display: true,
  };
}

export default function (pi: ExtensionAPI) {
  pi.on("resources_discover", async () => {
    return {
      skillPaths: [join(process.env.PI_CODING_AGENT_DIR ?? join(process.env.HOME ?? "", ".pi", "agent"), "skills")],
    };
  });

  pi.on("session_start", async (_event, ctx) => {
    ctx.ui.setStatus("mnemon", "mnemon");
  });

  pi.on("before_agent_start", async () => {
    const guide = readGuide();
    const content = [memoryStatus(), guide, "[mnemon] Evaluate: recall needed? After responding, evaluate: remember needed?"]
      .filter(Boolean)
      .join("\n\n");

    return { message: visibleMessage(content) };
  });

  pi.on("agent_end", async (_event, ctx) => {
    ctx.ui.notify("[mnemon] Consider whether this exchange warrants durable memory.", "info");
  });

  pi.on("session_before_compact", async () => {
    return {
      customInstructions: "[mnemon] Before compacting, preserve only critical continuity with mnemon remember when justified. Do not store the full transcript.",
    };
  });
}
