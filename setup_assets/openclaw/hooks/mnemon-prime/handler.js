import { existsSync, readFileSync } from "fs";
import { homedir } from "os";
import { join } from "path";
import { execSync } from "child_process";

const LEGACY_GUIDE_PATH = join(homedir(), ".mnemon", "prompt", "guide.md");

function guidePath() {
  const dataRoot = process.env.MNEMON_DATA_DIR || join(homedir(), ".mnemon");
  const scopedPath = join(dataRoot, "prompt", "guide.md");
  if (!existsSync(scopedPath) && existsSync(LEGACY_GUIDE_PATH)) {
    return LEGACY_GUIDE_PATH;
  }
  return scopedPath;
}

const handler = async (event) => {
  if (event.type !== "agent" || event.action !== "bootstrap") return;

  const parts = [];

  // Inject mnemon status summary
  try {
    const status = execSync("mnemon status 2>/dev/null", {
      timeout: 3000,
      encoding: "utf-8",
    });
    if (status) {
      const insights =
        status.match(/"total_insights":\s*(\d+)/)?.[1] || "0";
      const edges = status.match(/"edge_count":\s*(\d+)/)?.[1] || "0";
      parts.push(
        `[mnemon] Memory active (${insights} insights, ${edges} edges).`
      );
    }
  } catch {
    parts.push("[mnemon] Memory active.");
  }

  // Inject behavioral guide
  try {
    const guide = readFileSync(guidePath(), "utf-8");
    if (guide) parts.push(guide);
  } catch {
    // guide.md not found — skill-only mode, no guide injection
  }

  if (parts.length === 0) return;

  event.context.bootstrapFiles.push({
    name: "MNEMON-GUIDE.md",
    path: "mnemon/guide.md",
    content: parts.join("\n\n"),
    missing: false,
  });
};

export default handler;
