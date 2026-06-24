const MAX_RECALL_CHARS = 4000

function runMnemon(args, options = {}) {
  try {
    const proc = Bun.spawnSync(["mnemon", ...args], {
      stdout: "pipe",
      stderr: "pipe",
      env: process.env,
      cwd: options.cwd || process.cwd(),
    })
    if (!proc.success) return ""
    return new TextDecoder().decode(proc.stdout).trim()
  } catch {
    return ""
  }
}

function textFromPart(part) {
  if (!part || typeof part !== "object") return ""
  if (part.type === "text" && typeof part.text === "string") return part.text
  if (typeof part.content === "string") return part.content
  return ""
}

function lastUserMessage(output) {
  const messages = Array.isArray(output?.messages) ? output.messages : []
  for (let i = messages.length - 1; i >= 0; i--) {
    const msg = messages[i]
    const role = msg?.info?.role || msg?.role
    if (role !== "user") continue
    const parts = Array.isArray(msg.parts) ? msg.parts : []
    return { msg, parts, text: parts.map(textFromPart).filter(Boolean).join("\n") }
  }
  return null
}

function prependText(parts, text) {
  if (!Array.isArray(parts) || text.trim() === "") return
  parts.unshift({ type: "text", text })
}

function buildRecallContext(query, cwd) {
  const status = runMnemon(["status"], { cwd })
  const recall = query.trim() === "" ? "" : runMnemon(["recall", query, "--limit", "5"], { cwd })
  const sections = []
  if (status) sections.push(`Status:\n${status}`)
  if (recall) sections.push(`Relevant recall:\n${recall.slice(0, MAX_RECALL_CHARS)}`)
  sections.push("Use mnemon when it materially improves continuity. After responding, decide whether durable preferences, decisions, insights, facts, or context should be stored with mnemon remember/link.")
  return `\n\n<mnemon_context>\n${sections.join("\n\n")}\n</mnemon_context>\n\n`
}

export const MnemonPlugin = async ({ directory, client }) => {
  await client?.app?.log?.({
    body: {
      service: "mnemon",
      level: "info",
      message: "Mnemon OpenCode plugin loaded",
    },
  })

  return {
    "shell.env": async (_input, output) => {
      if (!output.env) output.env = {}
      output.env.MNEMON_OPENCODE = "1"
    },

    "experimental.chat.messages.transform": async (_input, output) => {
      const current = lastUserMessage(output)
      if (!current) return
      const marker = "<mnemon_context>"
      if (current.text.includes(marker)) return
      prependText(current.parts, buildRecallContext(current.text, directory))
    },

    "experimental.session.compacting": async (_input, output) => {
      if (!Array.isArray(output.context)) output.context = []
      output.context.push(`## Mnemon Memory

Before compaction completes, preserve durable preferences, decisions, insights, facts, or context with mnemon remember/link when they will improve future continuity. Do not store secrets, credentials, or short-lived operational noise.`)
    },

    event: async ({ event }) => {
      if (event?.type !== "session.idle") return
      await client?.app?.log?.({
        body: {
          service: "mnemon",
          level: "info",
          message: "OpenCode session idle; evaluate whether durable memory should be written with mnemon",
        },
      })
    },
  }
}
